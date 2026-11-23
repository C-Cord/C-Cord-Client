// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"


static bool drafts_path(char *out, size_t cap)
{
  const char *dir = ccord_paths_data_dir();
  if (!dir || !dir[0]) return false;
  ccord_error err;
  return ccord_paths_join(out, cap, dir, "drafts.json", &err) == CCORD_OK;
}

static bool drafts_ensure(ccord_ui *ui, size_t need)
{
  if (!ui) return false;
  if (need > UI_DRAFTS_MAX) need = UI_DRAFTS_MAX;
  if (need <= ui->cap_drafts) return true;
  size_t ncap = ui->cap_drafts ? ui->cap_drafts : 8u;
  while (ncap < need) ncap *= 2u;
  if (ncap > UI_DRAFTS_MAX) ncap = UI_DRAFTS_MAX;
  ui_draft *n = ccord_realloc_array(ui->drafts, ncap, sizeof *n);
  if (!n) return false;
  if (ncap > ui->cap_drafts) {
    memset(n + ui->cap_drafts, 0, (ncap - ui->cap_drafts) * sizeof *n);
  }
  ui->drafts = n;
  ui->cap_drafts = ncap;
  return true;
}

static bool draft_set_text(ui_draft *d, const char *s, size_t len)
{
  if (len > CCORD_SEND_MAX_BYTES) len = CCORD_SEND_MAX_BYTES;
  char *dup = ccord_strndup(s ? s : "", len);
  if (!dup) return false;
  ccord_free(d->text);
  d->text = dup;
  return true;
}

static void draft_remove_at(ccord_ui *ui, size_t i)
{
  ccord_free(ui->drafts[i].text);
  ui->drafts[i] = ui->drafts[--ui->ndrafts];
  ui->drafts[ui->ndrafts] = (ui_draft){0};
  if (ui->ndrafts == 0) {
    ccord_free(ui->drafts);
    ui->drafts = nullptr;
    ui->cap_drafts = 0;
  }
}

static void drafts_save(const ccord_ui *ui)
{
  if (!ui) return;
  const char *dir = ccord_paths_data_dir();
  if (!dir || !dir[0]) return;
  char path[1024];
  if (!drafts_path(path, sizeof path)) return;
  ccord_error derr;
  (void)ccord_mkdir_p(dir, 0700, &derr);
  ccord_buf b;
  ccord_buf_init(&b, 512);
  ccord_jw jw;
  ccord_jw_init(&jw, &b);
  ccord_jw_begin_object(&jw);
  for (size_t i = 0; ui->drafts && i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id == CCORD_ID_NONE || !ui->drafts[i].text || ui->drafts[i].text[0] == '\0') continue;
    char key[24];
    ccord_id_format(ui->drafts[i].channel_id, key);
    ccord_jw_key(&jw, key);
    ccord_jw_cstr(&jw, ui->drafts[i].text);
  }
  ccord_jw_end_object(&jw);
  if (ccord_jw_ok(&jw)) {
    FILE *f = fopen(path, "wb");
    if (f) {
      (void)fwrite(b.data, 1, b.len, f);
      fclose(f);
    }
  }
  ccord_buf_free(&b);
}

void uc_drafts_load(ccord_ui *ui)
{
  if (!ui) return;
  char path[1024];
  if (!drafts_path(path, sizeof path)) return;
  FILE *f = fopen(path, "rb");
  if (!f) return;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return;
  }
  long sz = ftell(f);
  if (sz <= 0 || sz > (long)(512u * 1024u) || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return;
  }
  char *buf = ccord_malloc((size_t)sz);
  if (!buf) {
    fclose(f);
    return;
  }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got == (size_t)sz) {
    ccord_arena arena;
    ccord_arena_init(&arena, 0);
    ccord_error jerr;
    const ccord_json *root = ccord_json_parse(&arena, buf, got, nullptr, &jerr);
    if (root && ccord_json_type_of(root) == CCORD_JSON_OBJECT) {
      size_t n = ccord_json_object_len(root);
      for (size_t i = 0; i < n && ui->ndrafts < UI_DRAFTS_MAX; i++) {
        const ccord_json *val = nullptr;
        const char *k = ccord_json_object_key(root, i, &val);
        ccord_id cid = CCORD_ID_NONE;
        if (!k || !ccord_id_parse(k, strlen(k), &cid)) continue;
        size_t vlen = 0;
        const char *v = ccord_json_string(val, &vlen);
        if (!v || vlen == 0 || !ccord_utf8_valid_text(v, vlen)) continue;
        if (!drafts_ensure(ui, ui->ndrafts + 1u)) break;
        if (vlen > CCORD_SEND_MAX_BYTES) vlen = CCORD_SEND_MAX_BYTES;
        char *copy = ccord_strndup(v, vlen);
        if (!copy) break;
        size_t slot = ui->ndrafts++;
        ui->drafts[slot].channel_id = cid;
        ui->drafts[slot].text = copy;
      }
    }
    ccord_arena_free(&arena);
  }
  ccord_free(buf);
}

void uc_save_channel_draft(ccord_ui *ui, ccord_id ch)
{
  if (!ui || ch == CCORD_ID_NONE || ui->editing_message != CCORD_ID_NONE) return;
  const char *txt = ccord_text_edit_cstr(&ui->edit);
  size_t len = ui->edit.len;
  for (size_t i = 0; ui->drafts && i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id == ch) {
      if (len == 0) {
        draft_remove_at(ui, i);
      } else {
        (void)draft_set_text(&ui->drafts[i], txt, len);
      }
      drafts_save(ui);
      return;
    }
  }
  if (len == 0) return;
  if (len > CCORD_SEND_MAX_BYTES) len = CCORD_SEND_MAX_BYTES;
  char *copy = ccord_strndup(txt, len);
  if (!copy) return;
  size_t slot = 0;
  if (ui->ndrafts < UI_DRAFTS_MAX) {
    if (!drafts_ensure(ui, ui->ndrafts + 1u)) {
      ccord_free(copy);
      return;
    }
    slot = ui->ndrafts++;
  } else {
    slot = UI_DRAFTS_MAX - 1u;
    ccord_free(ui->drafts[slot].text);
  }
  ui->drafts[slot].channel_id = ch;
  ui->drafts[slot].text = copy;
  drafts_save(ui);
}

void ui_clear_channel_draft(ccord_ui *ui, ccord_id channel_id)
{
  if (!ui || !ui->drafts || channel_id == CCORD_ID_NONE) return;
  for (size_t i = 0; i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id == channel_id) {
      draft_remove_at(ui, i);
      drafts_save(ui);
      return;
    }
  }
}

void ui_dismiss_ephemeral(ccord_ui *ui, ccord_id channel_id, ccord_id message_id)
{
  if (!ui || channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) return;
  ccord_error err;
  ccord_status st = ui->d.session
                      ? ccord_session_dismiss_ephemeral(ui->d.session, channel_id, message_id, &err)
                      : ccord_store_dismiss_ephemeral(ui->d.store, channel_id, message_id, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Cannot dismiss message: %s", err.message);
    return;
  }
  ui->view_key = 0;
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

uint32_t ui_slowmode_remaining_s(const ccord_ui *ui, const ccord_channel *ch)
{
  if (!ui || !ch || ch->rate_limit_per_user == 0 || ch->guild_id == CCORD_DM_GUILD_ID) return 0;
  if (ui->d.store && ch->guild_id != CCORD_ID_NONE) {
    ccord_id self = ccord_store_self_id(ui->d.store);
    if (ccord_store_permissions_exact(ui->d.store, ch->guild_id, self)) {
      uint64_t p = ccord_store_permissions(ui->d.store, ch->guild_id, ch->id, self, ui->now_unix_ms);
      uint64_t bypass = CCORD_PERM_BYPASS_SLOWMODE | CCORD_PERM_MANAGE_CHANNELS | CCORD_PERM_MANAGE_MESSAGES;
      if ((p & CCORD_PERM_ADMINISTRATOR) != 0 || (p & bypass) != 0) return 0;
    }
  }
  uint64_t win_ms = (uint64_t)ch->rate_limit_per_user * 1000ULL;
  for (size_t i = 0; i < ui->nslowmode_sent; i++) {
    if (ui->slowmode_sent[i].channel_id != ch->id) continue;
    uint64_t sent_ms = ui->slowmode_sent[i].sent_ms;
    if (sent_ms == 0 || ui->now_ms < sent_ms) return 0;
    uint64_t elapsed = ui->now_ms - sent_ms;
    if (elapsed >= win_ms) return 0;
    return (uint32_t)((win_ms - elapsed + 999ULL) / 1000ULL);
  }
  return 0;
}

void ui_record_slowmode_sent(ccord_ui *ui, const ccord_channel *ch)
{
  if (!ui || !ch || ch->rate_limit_per_user == 0 || ch->guild_id == CCORD_DM_GUILD_ID) return;
  uint64_t stamp = ui->now_ms != 0 ? ui->now_ms : 1ULL;
  for (size_t i = 0; i < ui->nslowmode_sent; i++) {
    if (ui->slowmode_sent[i].channel_id == ch->id) {
      ui->slowmode_sent[i].sent_ms = stamp;
      return;
    }
  }
  size_t slot = 0;
  if (ui->nslowmode_sent < 32u) {
    slot = ui->nslowmode_sent++;
  } else {
    for (size_t i = 1; i < 32u; i++) {
      if (ui->slowmode_sent[i].sent_ms < ui->slowmode_sent[slot].sent_ms) slot = i;
    }
  }
  ui->slowmode_sent[slot].channel_id = ch->id;
  ui->slowmode_sent[slot].sent_ms = stamp;
}

const ccord_message *uc_latest_non_ephemeral_message(const ccord_store *s, ccord_id ch)
{
  if (!s || ch == CCORD_ID_NONE) return nullptr;
  size_t n = ccord_store_message_count(s, ch);
  for (size_t i = n; i > 0; i--) {
    const ccord_message *m = ccord_store_message_at(s, ch, i - 1);
    // M0-3: pending/failed local echoes are not server messages; acks and read targets skip them.
    if (m && !ccord_message_is_ephemeral(m) && m->send_state == CCORD_MSG_SEND_CONFIRMED) return m;
  }
  return nullptr;
}

void uc_restore_channel_draft(ccord_ui *ui, ccord_id ch)
{
  if (!ui || ui->editing_message != CCORD_ID_NONE) return;
  ccord_text_edit_clear(&ui->edit);
  if (ch == CCORD_ID_NONE || !ui->drafts) return;
  for (size_t i = 0; i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id == ch && ui->drafts[i].text) {
      ccord_error err;
      size_t len = strlen(ui->drafts[i].text);
      (void)ccord_text_edit_insert(&ui->edit, ui->drafts[i].text, len, &err);
      return;
    }
  }
}

static const char *failure_verb(ccord_action_kind a)
{
  switch (a) {
    case CCORD_ACTION_SEND: return "send";
    case CCORD_ACTION_REPLY: return "reply";
    case CCORD_ACTION_EDIT: return "edit";
    case CCORD_ACTION_DELETE: return "delete";
    case CCORD_ACTION_REACTION: return "react";
    case CCORD_ACTION_PIN: return "pin";
    case CCORD_ACTION_UPLOAD: return "upload";
    case CCORD_ACTION_STICKER: return "send the sticker";
    case CCORD_ACTION_OPEN_DM: return "open the DM";
    case CCORD_ACTION_RELATIONSHIP: return "update that";
    case CCORD_ACTION_POLL_VOTE: return "vote";
    case CCORD_ACTION_CREATE_POLL: return "create poll";
    case CCORD_ACTION_REPORT_MENU: return "load the report form";
    case CCORD_ACTION_REPORT: return "send report";
    case CCORD_ACTION_INTERACTION: return "use that control";
    case CCORD_ACTION_VOICE: return "stay in the call";
    case CCORD_ACTION_ADMIN:   // reported through ccord_admin_op_verb, which knows which call it was
    case CCORD_ACTION_NONE: break;
  }
  return "do that";
}

// Prepends `draft` (then a newline) to the text in dst, which holds `*len` bytes in a buffer of `cap`.
// False when both together would not fit.
static bool prepend_text(char *dst, size_t cap, size_t *len, const char *draft, size_t dlen)
{
  size_t sep = *len > 0 ? 1u : 0u;
  if (dlen + sep + *len >= cap) return false;
  memmove(dst + dlen + sep, dst, *len + 1u);
  memcpy(dst, draft, dlen);
  if (sep) dst[dlen] = '\n';
  *len += dlen + sep;
  return true;
}

// A message that did not go through is the user's text: give it back rather than let it vanish. It goes into the
// composer when they are still in that channel (ahead of anything typed since), else into that channel's draft.
// Returns false only when both texts together would exceed the message limit.
static bool return_failed_draft(ccord_ui *ui, const ccord_action_failure *f)
{
  if (!f->draft || f->draft_len == 0 || f->draft_len > CCORD_SEND_MAX_BYTES) return true;
  ccord_id selected = ccord_store_selected_channel(ui->d.store);
  ccord_error err;
  if (f->channel_id == selected && ui->editing_message == CCORD_ID_NONE) {
    if (f->action == CCORD_ACTION_REPLY && ui->reply_message == CCORD_ID_NONE &&
        ccord_store_find_message(ui->d.store, f->channel_id, f->message_id)) {
      ui->reply_channel = f->channel_id;
      ui->reply_message = f->message_id;
    }
    char cur[CCORD_SEND_MAX_BYTES + 1];
    size_t clen = ui->edit.len < sizeof cur - 1u ? ui->edit.len : sizeof cur - 1u;
    memcpy(cur, ccord_text_edit_cstr(&ui->edit), clen);
    cur[clen] = '\0';
    if (!prepend_text(cur, sizeof cur, &clen, f->draft, f->draft_len)) return false;
    ccord_text_edit_clear(&ui->edit);
    if (ccord_text_edit_insert(&ui->edit, cur, clen, &err) != CCORD_OK) return false;
    ccord_text_edit_set_caret(&ui->edit, ui->edit.len, false);
    ui->composer_scroll = 0.0f;
    ui->caret_epoch_ms = ui->now_ms;
    ui->caret_on = true;
    return true;
  }
  for (size_t i = 0; ui->drafts && i < ui->ndrafts; i++) {
    if (ui->drafts[i].channel_id != f->channel_id) continue;
    char buf[CCORD_SEND_MAX_BYTES + 1];
    size_t len = ui->drafts[i].text ? strlen(ui->drafts[i].text) : 0;
    if (len > CCORD_SEND_MAX_BYTES) len = CCORD_SEND_MAX_BYTES;
    if (len > 0) memcpy(buf, ui->drafts[i].text, len);
    buf[len] = '\0';
    bool ok = prepend_text(buf, sizeof buf, &len, f->draft, f->draft_len);
    if (ok && draft_set_text(&ui->drafts[i], buf, len)) drafts_save(ui);
    else ok = false;
    return ok;
  }
  char *copy = ccord_strndup(f->draft, f->draft_len);
  if (!copy) return false;
  size_t slot = 0;
  if (ui->ndrafts < UI_DRAFTS_MAX) {
    if (!drafts_ensure(ui, ui->ndrafts + 1u)) {
      ccord_free(copy);
      return false;
    }
    slot = ui->ndrafts++;
  } else {
    slot = UI_DRAFTS_MAX - 1u;
    ccord_free(ui->drafts[slot].text);
  }
  ui->drafts[slot].channel_id = f->channel_id;
  ui->drafts[slot].text = copy;
  drafts_save(ui);
  return true;
}

// A failed edit goes back into edit mode on the same message with the text the user wrote, but only when that cannot
// clobber anything they are doing now (same channel, composer empty, not editing another message).
static bool return_failed_edit(ccord_ui *ui, const ccord_action_failure *f)
{
  if (!f->draft || f->draft_len == 0) return true;
  if (f->channel_id != ccord_store_selected_channel(ui->d.store) || ui->editing_message != CCORD_ID_NONE ||
      ui->edit.len != 0 || ui->reply_message != CCORD_ID_NONE) {
    return false;
  }
  ui_begin_edit(ui, f->channel_id, f->message_id);
  if (ui->editing_message != f->message_id) return false;
  ccord_error err;
  ccord_text_edit_clear(&ui->edit);
  if (ccord_text_edit_insert(&ui->edit, f->draft, f->draft_len, &err) != CCORD_OK) {
    ui_cancel_edit(ui);
    return false;
  }
  ccord_text_edit_set_caret(&ui->edit, ui->edit.len, false);
  return true;
}

// Actions run in the background, so a refusal arrives after the call returned: say so, and hand back the text.
void ui_drain_failures(ccord_ui *ui)
{
  if (!ui->d.session) return;
  ccord_action_failure f;
  while (ccord_session_pop_failure(ui->d.session, &f)) {
    if (f.action == CCORD_ACTION_ADMIN) {   // nothing to hand back; the dialog that started it is long gone
      ui_notice(ui, "Couldn't %s: %s", ccord_admin_op_verb(f.admin_op), f.reason);
      ui_server_settings_on_failure(ui, f.admin_op);
      ui_channel_settings_on_failure(ui, f.admin_op);
      ui_status_card_on_failure(ui, f.admin_op);
      ccord_action_failure_free(&f);
      ui->dirty = true;
      continue;
    }
    if (f.action == CCORD_ACTION_VOICE) {   // reconnect gave up; shown once per failure event
      ui_notice(ui, "Disconnected from voice: %s", f.reason);
      ccord_action_failure_free(&f);
      ui->dirty = true;
      continue;
    }
    if (f.action == CCORD_ACTION_REPORT || f.action == CCORD_ACTION_REPORT_MENU) {   // no draft: the flow keeps the form
      (void)ui_report_flow_on_failure(ui, &f);
      ccord_action_failure_free(&f);
      ui->dirty = true;
      continue;
    }
    if (f.action == CCORD_ACTION_CREATE_POLL) {   // no text draft: the composer keeps the form and reopens it
      ui_notice(ui, "Couldn't create poll: %s", f.reason);
      (void)ui_poll_composer_on_refused(ui, f.reason);
      ccord_action_failure_free(&f);
      ui->dirty = true;
      continue;
    }
    bool restored = f.action == CCORD_ACTION_EDIT ? return_failed_edit(ui, &f) : return_failed_draft(ui, &f);
    if (restored) ui_notice(ui, "Couldn't %s: %s", failure_verb(f.action), f.reason);
    else ui_notice(ui, "Couldn't %s: %s (your text could not be restored)", failure_verb(f.action), f.reason);
    ccord_action_failure_free(&f);
    ui->dirty = true;
  }
}

void ui_free_inter_modal(ccord_ui *ui)
{
  if (!ui) return;
  if (ui->inter_modal_edits) {
    for (size_t i = 0; i < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
      ccord_text_edit_free(&ui->inter_modal_edits[i]);
    }
    ccord_free(ui->inter_modal_edits);
    ui->inter_modal_edits = nullptr;
  }
  ccord_free(ui->inter_modal);
  ui->inter_modal = nullptr;
}

void ui_drain_modals(ccord_ui *ui)
{
  if (!ui->d.session) return;
  ccord_interaction_modal m;
  if (ccord_session_pop_interaction_modal(ui->d.session, &m)) {
    if (!ui->inter_modal) {
      ui->inter_modal = ccord_calloc(1, sizeof *ui->inter_modal);
    }
    if (!ui->inter_modal_edits) {
      ui->inter_modal_edits = ccord_calloc(CCORD_RICH_MAX_COMPONENT_ROWS, sizeof *ui->inter_modal_edits);
      if (ui->inter_modal_edits) {
        for (size_t i = 0; i < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
          ccord_text_edit_init(&ui->inter_modal_edits[i], 4000);
        }
      }
    }
    if (ui->inter_modal && ui->inter_modal_edits) {
      *ui->inter_modal = m;
      for (size_t i = 0; i < CCORD_RICH_MAX_COMPONENT_ROWS; i++) {
        ccord_text_edit_clear(&ui->inter_modal_edits[i]);
        if (i < m.field_count && m.fields[i].value[0] != '\0') {
          ccord_error ierr;
          (void)ccord_text_edit_insert(&ui->inter_modal_edits[i], m.fields[i].value, strlen(m.fields[i].value), &ierr);
        }
      }
      ui->inter_modal_focus = 0;
      ui_close_select_dropdown(ui);
      ui->modal_kind = UI_MODAL_INTERACTION;
      ui->dirty = true;
    } else {
      ui_free_inter_modal(ui);
    }
  }
  ui_report_flow_pump(ui);
}

void ui_dismiss_new_marker(ccord_ui *ui, bool ack_channel)
{
  if (!ui) return;
  if (ack_channel) {
    ccord_id ch = ccord_store_selected_channel(ui->d.store);
    const ccord_channel *c = ccord_store_channel(ui->d.store, ch);
    const ccord_message *last = uc_latest_non_ephemeral_message(ui->d.store, ch);
    ccord_id target_id = last ? last->id : (c ? c->last_message_id : CCORD_ID_NONE);
    if (c && target_id != CCORD_ID_NONE && (c->unread_count > 0 || c->mention_count > 0)) {
      ccord_error err;
      if (ui->d.session) {
        if (ccord_session_ack_message(ui->d.session, ch, target_id, &err) == CCORD_OK) {
          ui->acked_message = target_id;
          ui->ack_channel = ch;
          ui->ack_next_ms = ui->now_ms + UI_ACK_MIN_GAP_MS;
        }
      } else {
        (void)ccord_store_apply_read(ui->d.store, ch, target_id, 0, &err);
      }
    }
  }
  if (ui->new_marker_active) {
    ui->new_marker_active = false;
    ui->new_marker_channel = CCORD_ID_NONE;
    ui->new_marker_id = CCORD_ID_NONE;
    ui->new_marker_count = 0;
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  ui->dirty = true;
}

bool ui_jump_to_message(ccord_ui *ui, ccord_id target_msg_id)
{
  if (!ui || target_msg_id == CCORD_ID_NONE) return false;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  if (ch == CCORD_ID_NONE) return false;
  // The timeline is identity-mapped even while search is open, so the target is always in view.
  size_t count = ui_view_count(ui);
  for (size_t i = 0; i < count; i++) {
    size_t raw = (size_t)ui_view_index(ui, i);
    if ((raw & (1ULL << 62)) != 0) continue;
    const ccord_message *m = ccord_store_message_at(ui->d.store, ch, raw);
    if (m && m->id == target_msg_id) {
      ui_blocked_reveal_for(ui, ch, raw);   // a jump into a collapsed run opens the run
      ui->msg_scroll_vel = 0.0f;
      ui->vlist.stick_bottom = false;
      ui->vlist.anchor_index = i;
      ui->vlist.anchor_offset = fmaxf(0.0f, ui->layout.message_area.h * 0.25f);
      ui->jump_highlight_msg_id = target_msg_id;
      ui->jump_highlight_until_ms = ui->now_ms + UI_JUMP_FLASH_MS;
      ui->dirty = true;
      return true;
    }
  }
  const ccord_message *first = count > 0 ? ccord_store_message_at(ui->d.store, ch, 0) : nullptr;
  if (first && target_msg_id < first->id && ui->d.session && ccord_session_history_has_more(ui->d.session, ch)) {
    ui->pending_jump_msg_id = target_msg_id;
    ui->pending_jump_channel = ch;
    ccord_error herr;
    if (ccord_session_request_history_before(ui->d.session, ch, first->id, &herr) == CCORD_OK) {
      ui->hist_channel = ch;
      ui->hist_before = first->id;
      ui->hist_request_ms = ui->now_ms ? ui->now_ms : 1;
      ui_notice(ui, "Loading older messages...");
      return true;
    }
  }
  ui_notice(ui, "Referenced message is not available");
  return false;
}

// ---- Scroll anchor by message id ----
// vlist.anchor_index is an index into the message list, and the timeline is identity-mapped to the store. When the
// store replaces a page under the reader (a cached page is reconciled with the live one: a deleted message goes, a newer
// one arrives) the index would point at another message. The id and index of the anchor message are remembered at the
// end of every drawn frame, and when the list changes the index is moved back onto that message. Skipped when the
// anchor was moved by the user in between (a jump), so the jump wins. One UI per process, like the settings.
static struct {
  const ccord_ui *owner;
  ccord_id channel, msg_id;
  size_t index;
} g_anchor;

void ui_anchor_remember(ccord_ui *ui)
{
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  g_anchor.owner = ui;
  g_anchor.channel = ch;
  g_anchor.msg_id = CCORD_ID_NONE;
  if (ch == CCORD_ID_NONE || ui->vlist.stick_bottom || ui->vlist.count == 0) return;
  if (ui->vlist.count != ccord_store_message_count(ui->d.store, ch)) return;
  const ccord_message *m = ccord_store_message_at(ui->d.store, ch, ui->vlist.anchor_index);
  if (!m) return;
  g_anchor.msg_id = m->id;
  g_anchor.index = ui->vlist.anchor_index;
}

// Where the remembered anchor message is now, or SIZE_MAX when it is gone or there is nothing to restore.
static size_t ui_anchor_target(const ccord_ui *ui, ccord_id ch)
{
  if (g_anchor.owner != ui || g_anchor.channel != ch || g_anchor.msg_id == CCORD_ID_NONE) return SIZE_MAX;
  if (ui->vlist.anchor_index != g_anchor.index) return SIZE_MAX;
  size_t lo = 0, hi = ccord_store_message_count(ui->d.store, ch);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const ccord_message *m = ccord_store_message_at(ui->d.store, ch, mid);
    if (!m) return SIZE_MAX;
    if (m->id == g_anchor.msg_id) return mid;
    if (m->id < g_anchor.msg_id) lo = mid + 1;
    else hi = mid;
  }
  return SIZE_MAX;
}

// Brings the virtual list in line with the store: channel switch -> jump to bottom; count change -> grow/shrink.
void ui_sync_messages(ccord_ui *ui)
{
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  uint64_t rev = ccord_store_revision(ui->d.store);
  uint64_t svk = ui->view_key ^ ((uint64_t)ui->nignored_users << 48) ^
                 (ui->discovery_open ? 2ull : 0ull) ^ (ui->dm_friends_view ? 4ull : 0ull) ^
                 (ui->dm_requests_view ? 8ull : 0ull) ^ (ui->new_marker_active ? 16ull : 0ull);
  for (size_t i = 0; i < ui->nignored_users; i++) svk = uc_sig_mix_u64(svk, ui->ignored_users[i]);
  if (rev == ui->synced_msg_rev && ch == ui->synced_msg_ch && ch == ui->vlist_channel &&
      !ui->search_open && ui->view_key != 0 && svk == ui->synced_view_key &&
      ui->nav_hist_len > 0 &&
      ui->pending_jump_msg_id == CCORD_ID_NONE && ui->stage_channel == CCORD_ID_NONE &&
      ui->vlist.count == ccord_store_message_count(ui->d.store, ch)) {
    return;
  }
  bool switched = ch != ui->vlist_channel;
  const ccord_channel *ch_obj = ccord_store_channel(ui->d.store, ch);
  if (switched) {
    if (ui->editing_message == CCORD_ID_NONE) {
      if (ui->vlist_channel != CCORD_ID_NONE) {
        uc_save_channel_draft(ui, ui->vlist_channel);
        uc_restore_channel_draft(ui, ch);
      } else if (ui->edit.len == 0) {
        uc_restore_channel_draft(ui, ch);
      }
    }
    ui->vlist_channel = ch;
    ui->msg_scroll_vel = 0.0f;
    ui->chat_sel_active = false;
    ui->chat_sel_len = 0;
    ui->chat_sel_buf[0] = '\0';
    ui_close_select_dropdown(ui);
    ui->autocomplete_open = false;
    ui->nautocomplete = 0;
    ui->slash_auto_due_ms = 0;
    ccord_vlist_reset_bottom(&ui->vlist);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->typing_channel = CCORD_ID_NONE;
    ui->last_typing_ms = 0;
    ui->acked_message = CCORD_ID_NONE;
    ui->ack_channel = CCORD_ID_NONE;
    ui->ack_next_ms = ui->now_ms + UI_ACK_SETTLE_MS;   // flipping through channels does not ack each one
    ui->scroll_up_pending = false;
    ui->hist_channel = ui->hist_before = CCORD_ID_NONE;
    ui->hist_request_ms = 0;
    ui->jump_highlight_msg_id = CCORD_ID_NONE;
    ui->jump_highlight_until_ms = 0;
    ui->nmb_seek_channel = CCORD_ID_NONE;
    ui->pending_jump_msg_id = ui->pending_jump_channel = CCORD_ID_NONE;
    if (ch_obj && ch_obj->unread_count > 0) {
      ui->new_marker_active = true;
      ui->new_marker_channel = ch;
      ui->new_marker_id = ch_obj->last_read_id;
      ui->new_marker_count = ch_obj->unread_count;
    } else {
      ui->new_marker_active = false;
      ui->new_marker_channel = CCORD_ID_NONE;
      ui->new_marker_id = CCORD_ID_NONE;
      ui->new_marker_count = 0;
    }
    if (ui->editing_message != CCORD_ID_NONE && ui->editing_channel != ch) ui_cancel_edit(ui);
    if (ui->reply_message != CCORD_ID_NONE && ui->reply_channel != ch) ui_cancel_reply(ui);
    ui->dirty = true;
  } else if (ch_obj && ch_obj->unread_count > 0) {
    if (!ui->new_marker_active || ui->new_marker_channel != ch) {
      ui->new_marker_active = true;
      ui->new_marker_channel = ch;
      ui->new_marker_id = ch_obj->last_read_id;
      ui->new_marker_count = ch_obj->unread_count;
      ccord_vlist_invalidate_all(&ui->vlist);
      ui->dirty = true;
    } else if (ch_obj->unread_count > ui->new_marker_count) {
      ui->new_marker_count = ch_obj->unread_count;
    }
  }
  if (ui->new_marker_active && ui->new_marker_channel == ch) {
    size_t total_m = ccord_store_message_count(ui->d.store, ch);
    if (ui->new_marker_id == CCORD_ID_NONE && ui->new_marker_count > 0 && total_m > ui->new_marker_count) {
      const ccord_message *prev_m = ccord_store_message_at(ui->d.store, ch, total_m - ui->new_marker_count - 1);
      if (prev_m && prev_m->id != CCORD_ID_NONE) {
        ui->new_marker_id = prev_m->id;
        ccord_vlist_invalidate_all(&ui->vlist);
        ui->dirty = true;
      }
    }
    ccord_id self_id = ccord_store_self_id(ui->d.store);
    if (total_m > 0 && self_id != CCORD_ID_NONE) {
      const ccord_message *tail_m = uc_latest_non_ephemeral_message(ui->d.store, ch);
      if (tail_m && tail_m->author_id == self_id &&
          (ui->new_marker_id == CCORD_ID_NONE || tail_m->id > ui->new_marker_id)) {
        ui->new_marker_active = false;
        ui->new_marker_channel = CCORD_ID_NONE;
        ui->new_marker_id = CCORD_ID_NONE;
        ui->new_marker_count = 0;
        ccord_vlist_invalidate_all(&ui->vlist);
        ui->dirty = true;
      }
    }
  }
  if (ui->editing_message != CCORD_ID_NONE && !ccord_store_find_message(ui->d.store, ui->editing_channel, ui->editing_message)) {
    ui_cancel_edit(ui);
  }
  if (ui->reply_message != CCORD_ID_NONE && !ccord_store_find_message(ui->d.store, ui->reply_channel, ui->reply_message)) {
    ui_cancel_reply(ui);
  }
  uint64_t bsig = ui_blocked_signature(ui);   // blocking or unblocking (here or on another device) re-lays the rows
  if (bsig != ui->blocked_sig) {
    ui->blocked_sig = bsig;
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
  }
  // Search only rebuilds the side panel's vmap; the timeline keeps its content and scroll position.
  ui_rebuild_view(ui, false);
  size_t count = ui_view_count(ui);
  size_t anchor_to = ui_anchor_target(ui, ch);
  if (count != ui->vlist.count) {
    size_t old_count = ui->vlist.count;
    bool was_empty = old_count == 0;
    ccord_error err;
    if (ccord_vlist_set_count(&ui->vlist, count, &err) != CCORD_OK) {
      ui_notice(ui, "%s", err.message);
    }
    if ((was_empty && count > 0) || ui->vlist.stick_bottom) {
      ccord_vlist_reset_bottom(&ui->vlist);
    } else if (anchor_to != SIZE_MAX) {
      ui->vlist.anchor_index = anchor_to;   // keep the message the reader was on, wherever it moved to
      ccord_vlist_invalidate_all(&ui->vlist);
    } else if (count > old_count) {
      ui->vlist.anchor_index += count - old_count;
      ccord_vlist_invalidate_all(&ui->vlist);
    }
    ui->dirty = true;
  } else if (anchor_to != SIZE_MAX && anchor_to != ui->vlist.anchor_index && !ui->vlist.stick_bottom) {
    // Same number of messages but a different set (a page was reconciled): follow the message, not the index.
    ui->vlist.anchor_index = anchor_to;
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
  }
  if (ui->pending_jump_msg_id != CCORD_ID_NONE && ui->pending_jump_channel == ch &&
      ccord_store_find_message(ui->d.store, ch, ui->pending_jump_msg_id) != nullptr) {
    ccord_id jid = ui->pending_jump_msg_id;
    ui->pending_jump_msg_id = CCORD_ID_NONE;
    ui->pending_jump_channel = CCORD_ID_NONE;
    (void)ui_jump_to_message(ui, jid);
  }
  if (ui->stage_channel != CCORD_ID_NONE) {
    const ccord_voice_state *v = ui_self_voice(ui);
    if (v && v->channel_id == ui->stage_channel) {
      ui->stage_armed = true;
    } else if (ui->stage_armed) {
      ui_clear_voice_stage(ui);   // disconnected or switched channels elsewhere
    } else if (!ccord_store_channel(ui->d.store, ui->stage_channel)) {
      ui_clear_voice_stage(ui);
    }
  }
  ui_record_nav_location(ui);
  ui->synced_msg_rev = ccord_store_revision(ui->d.store);
  ui->synced_msg_ch = ch;
  uint64_t end_svk = ui->view_key ^ ((uint64_t)ui->nignored_users << 48) ^
                     (ui->discovery_open ? 2ull : 0ull) ^ (ui->dm_friends_view ? 4ull : 0ull) ^
                     (ui->dm_requests_view ? 8ull : 0ull) ^ (ui->new_marker_active ? 16ull : 0ull);
  for (size_t i = 0; i < ui->nignored_users; i++) end_svk = uc_sig_mix_u64(end_svk, ui->ignored_users[i]);
  ui->synced_view_key = end_svk;
}

// The user is reading the open channel only while the window has focus, no full-window overlay covers the chat,
// and the list is at its newest message. Away, covered or scrolled up, what arrives there is unread (as in Discord),
// and it is not acknowledged either.
bool ui_is_reading(const ccord_ui *ui)
{
  if (!ui || !ui->window_focused || !ui->vlist.stick_bottom) return false;
  if (ui->settings_open || ui_server_settings_is_open(ui) || ui_channel_settings_is_open(ui) ||
      ui->discovery_open || ui->stream_fullscreen || ui_is_dm_friends_view(ui) || ui_is_dm_requests_view(ui) ||
      (ui->mobile_layout && (ui->left_drawer_open || ui->right_drawer_open))) {
    return false;
  }
  if (ui->stage_channel != CCORD_ID_NONE && !ui_stage_chat_active(ui)) return false;
  const ccord_channel *c = ui_selected_channel(ui);
  if (c && c->is_message_request) return false;
  return true;
}

bool ccord_ui_is_reading(const ccord_ui *ui) { return ui_is_reading(ui); }

bool ccord_ui_sound_kind_enabled(const ccord_ui *ui, unsigned kind)
{
  return ui_sound_kind_enabled(ui, kind);
}

ccord_id ccord_ui_stage_channel(const ccord_ui *ui)
{
  return ui ? ui->stage_channel : CCORD_ID_NONE;
}

void ui_sync_reading(ccord_ui *ui)
{
  if (!ui || !ui->d.session) return;
  ccord_session_set_reading(ui->d.session, ui_is_reading(ui) ? ccord_store_selected_channel(ui->d.store) : CCORD_ID_NONE);
}

// The server sends a big member list a hundred positions at a time. While the list is on screen in a focused window and its
// end is in sight (or it does not even fill the panel), the next piece is asked for; the session paces the asking (one
// piece in flight, one a second at most, a cap on the whole) and knows when the list is complete.
void uc_page_members(ccord_ui *ui)
{
  if (!ui->d.session || !ui->window_focused || !ui->layout.member_list_visible) return;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (g == CCORD_ID_NONE || g == CCORD_DM_GUILD_ID) return;
  const float lookahead = 3.0f * M(member_item_height);
  if (ui->member_scroll + ui->layout.member_list.h + lookahead < ui->member_content_h) return;
  (void)ccord_session_more_members(ui->d.session, ui->now_ms);
}

// Acknowledges the newest message while the list is pinned to the bottom of a focused window. One ack covers everything
// up to its message id, so a busy channel is acknowledged at most once per UI_ACK_MIN_GAP_MS (the newest message goes
// out when the gap has passed) rather than once per incoming message; a channel just switched to settles first.
void uc_auto_ack(ccord_ui *ui)
{
  if (!ui->d.session) return;
  ui_sync_reading(ui);
  if (!ui_is_reading(ui) || ccord_session_is_stale(ui->d.session)) return;
  ccord_id ch = ccord_store_selected_channel(ui->d.store);
  const ccord_channel *c = ccord_store_channel(ui->d.store, ch);
  if (!c) return;
  const ccord_message *m = uc_latest_non_ephemeral_message(ui->d.store, ch);
  ccord_id target_id = m ? m->id : CCORD_ID_NONE;
  bool hist_loaded = ccord_store_history_loaded(ui->d.store, ch);
  if (hist_loaded && c->last_message_id > target_id) {
    target_id = c->last_message_id;
  }
  if (target_id == CCORD_ID_NONE) return;
  if (!hist_loaded && c->last_message_id > target_id) return;
  if (c->last_read_id != CCORD_ID_NONE && target_id < c->last_read_id) {
    if (c->unread_count == 0 && c->mention_count == 0) return;
    target_id = c->last_read_id;   // clear stale unread/mention counts without rolling last_read_id backward
  }
  if (c->unread_count == 0 && c->mention_count == 0 &&
      (c->last_read_id == CCORD_ID_NONE || target_id <= c->last_read_id)) {
    ui->ack_channel = ch;
    ui->acked_message = target_id;
    return;
  }
  if (ui->ack_channel == ch && ui->acked_message == target_id) return;
  if (ui->now_ms < ui->ack_next_ms) return;
  ccord_error err;
  if (ccord_session_ack_message(ui->d.session, ch, target_id, &err) == CCORD_OK) {
    ui->acked_message = target_id;
    ui->ack_channel = ch;
    ui->ack_next_ms = ui->now_ms + UI_ACK_MIN_GAP_MS;
    ui->dirty = true;
  } else {
    ui->ack_next_ms = ui->now_ms + UI_ACK_RETRY_MS;
  }
}

// ---------------------------------------------------------------- search

static uint64_t search_key(const ccord_ui *ui, ccord_id guild, ccord_id channel)
{
  uint64_t h = 1469598103934665603ull;
  const char *q = ccord_text_edit_cstr(&ui->search_edit);
  for (size_t i = 0; i < ui->search_edit.len; i++) h = (h ^ (uint8_t)q[i]) * 1099511628211ull;
  h = (h ^ guild) * 1099511628211ull;
  h = (h ^ channel) * 1099511628211ull;
  return h ? h : 1u;
}

// Sends the typed query to Discord. `explicit_enter` is the user asking (Enter); a query that already went out is
// not sent again unless the user asks and it has been a while.
void uc_search_send(ccord_ui *ui, bool explicit_enter)
{
  ui->search_due_ms = 0;
  if (!ui->d.session || ui->search_edit.len == 0) return;
  if (!explicit_enter && ui->search_edit.len < 2) return;
  ccord_id sg = ccord_store_selected_guild(ui->d.store);
  ccord_id sch = ccord_store_selected_channel(ui->d.store);
  uint64_t key = search_key(ui, sg, sch);
  if (key == ui->search_sent_key && (!explicit_enter || ui->now_ms < ui->search_sent_ms + UI_SEARCH_RESEND_MS)) return;
  ccord_error serr;
  ui->search_sent_key = key;
  ui->search_sent_ms = ui->now_ms;
  ui->search_panel_scroll = 0.0f;
  ccord_id req_g = (sg != CCORD_ID_NONE && sg != CCORD_DM_GUILD_ID) ? sg : CCORD_ID_NONE;
  ccord_id req_ch = (req_g != CCORD_ID_NONE) ? CCORD_ID_NONE : sch;
  (void)ccord_session_search_messages_page(ui->d.session, req_g, req_ch, ccord_text_edit_cstr(&ui->search_edit),
                                           ui->search_edit.len, 0u, &serr);
}

void ui_search_page_step(ccord_ui *ui, int dir)
{
  if (!ui || !ui->d.session || ui->search_edit.len == 0) return;
  uint32_t cur = ccord_session_search_offset(ui->d.session);
  uint32_t total = ccord_session_search_total_results(ui->d.session);
  uint32_t next = cur;
  if (dir < 0) {
    if (cur == 0u) return;
    next = cur >= CCORD_SEARCH_PAGE_SIZE ? (cur - CCORD_SEARCH_PAGE_SIZE) : 0u;
  } else if (dir > 0) {
    if (cur + CCORD_SEARCH_PAGE_SIZE >= total) return;
    next = cur + CCORD_SEARCH_PAGE_SIZE;
  } else {
    return;
  }
  ccord_id sg = ccord_store_selected_guild(ui->d.store);
  ccord_id sch = ccord_store_selected_channel(ui->d.store);
  ccord_id req_g = (sg != CCORD_ID_NONE && sg != CCORD_DM_GUILD_ID) ? sg : CCORD_ID_NONE;
  ccord_id req_ch = (req_g != CCORD_ID_NONE) ? CCORD_ID_NONE : sch;
  ccord_error serr;
  ui->search_panel_scroll = 0.0f;
  (void)ccord_session_search_messages_page(ui->d.session, req_g, req_ch, ccord_text_edit_cstr(&ui->search_edit),
                                           ui->search_edit.len, next, &serr);
  ui->dirty = true;
}

void uc_search_reset(ccord_ui *ui)
{
  ui->search_due_ms = 0;
  ui->search_sent_key = 0;
}

// The query changed: it goes out once the user pauses typing, not once per keystroke.
void uc_search_schedule(ccord_ui *ui)
{
  if (ui->search_edit.len >= 2) {
    ui->search_due_ms = ui->now_ms + UI_SEARCH_DEBOUNCE_MS;
  } else {
    uc_search_reset(ui);
  }
}

void uc_search_tick(ccord_ui *ui)
{
  if (ui->search_due_ms == 0) return;
  if (!ui->search_open) {
    ui->search_due_ms = 0;
    return;
  }
  if (ui->now_ms >= ui->search_due_ms) uc_search_send(ui, false);
}

