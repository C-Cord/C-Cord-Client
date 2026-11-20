// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "server_settings_internal.h"

// ---------------------------------------------------------------- actions

void ss_copy_text(ccord_ui *ui, const char *text, const char *notice)
{
  if (!ui->d.host.clipboard_set) {
    ui_notice(ui, "Clipboard is not available");
    return;
  }
  ui->d.host.clipboard_set(ui->d.host.ud, text);
  ui_notice(ui, "%s", notice);
}

void ss_submit(ccord_ui *ui, ccord_status st, const ccord_admin_request *req, const ccord_error *err, const char *what)
{
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", what, err->message);
    return;
  }
  ui_admin_submit(ui, req);
}

static const char *ss_sniff_image_mime(const uint8_t *buf, size_t len)
{
  if (len >= 8u && memcmp(buf, "\x89PNG\r\n\x1a\n", 8u) == 0) return "image/png";
  if (len >= 3u && buf[0] == 0xFFu && buf[1] == 0xD8u && buf[2] == 0xFFu) return "image/jpeg";
  if (len >= 6u && (memcmp(buf, "GIF87a", 6u) == 0 || memcmp(buf, "GIF89a", 6u) == 0)) return "image/gif";
  if (len >= 12u && memcmp(buf, "RIFF", 4u) == 0 && memcmp(buf + 8u, "WEBP", 4u) == 0) return "image/webp";
  return nullptr;
}

static char *ss_encode_data_uri(const char *mime, const uint8_t *data, size_t len)
{
  char prefix[40];
  int plen = snprintf(prefix, sizeof prefix, "data:%s;base64,", mime);
  if (plen <= 0 || (size_t)plen >= sizeof prefix) return nullptr;
  size_t b64_cap = ccord_base64_encoded_cap(len);
  char *out = ccord_malloc((size_t)plen + b64_cap);
  if (!out) return nullptr;
  memcpy(out, prefix, (size_t)plen);
  (void)ccord_base64_encode(data, len, out + plen, b64_cap);
  return out;
}

void ss_pick_overview_image(ccord_ui *ui, bool banner)
{
  if (!ui->d.host.open_file_dialog) {
    ui_notice(ui, "File picker is not available");
    return;
  }
  char path[512] = "";
  if (!ui->d.host.open_file_dialog(ui->d.host.ud, path, sizeof path) || path[0] == '\0') return;
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    ui_notice(ui, "Cannot open image: %s", path);
    return;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    ui_notice(ui, "Cannot read image size");
    return;
  }
  long sz = ftell(fp);
  if (sz <= 0 || (unsigned long)sz > SS_IMAGE_MAX_BYTES || fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    ui_notice(ui, "Image must be between 1 B and 48 KB");
    return;
  }
  uint8_t *raw = ccord_malloc((size_t)sz);
  if (!raw) {
    fclose(fp);
    return;
  }
  size_t got = fread(raw, 1u, (size_t)sz, fp);
  fclose(fp);
  if (got != (size_t)sz) {
    ccord_free(raw);
    ui_notice(ui, "Failed to read image");
    return;
  }
  const char *mime = ss_sniff_image_mime(raw, got);
  if (!mime) {
    ccord_free(raw);
    ui_notice(ui, "Choose a PNG, JPEG, GIF, or WebP image");
    return;
  }
  char *uri = ss_encode_data_uri(mime, raw, got);
  ccord_free(raw);
  if (!uri) {
    ui_notice(ui, "Out of memory encoding image");
    return;
  }
  struct ui_server_settings *ss = ui->ss;
  if (banner) {
    ccord_free(ss->ov_banner_uri);
    ss->ov_banner_uri = uri;
    ss->ov_banner_mode = SS_IMG_SET;
  } else {
    ccord_free(ss->ov_icon_uri);
    ss->ov_icon_uri = uri;
    ss->ov_icon_mode = SS_IMG_SET;
  }
}

void ss_clear_overview_image(ccord_ui *ui, bool banner)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  if (banner) {
    ccord_free(ss->ov_banner_uri);
    ss->ov_banner_uri = nullptr;
    ss->ov_banner_mode = gd->banner_url[0] != '\0' ? SS_IMG_CLEAR : SS_IMG_UNCHANGED;
  } else {
    ccord_free(ss->ov_icon_uri);
    ss->ov_icon_uri = nullptr;
    ss->ov_icon_mode = (gd->icon_url && gd->icon_url[0] != '\0') ? SS_IMG_CLEAR : SS_IMG_UNCHANGED;
  }
}

void ss_save(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss_dirty(ss)) return;
  if (ss->last_save_ms && ui->now_ms < ss->last_save_ms + 800u) return;   // one press, one request
  ccord_admin_request req;
  ccord_error err;
  if (ss->page == SS_OVERVIEW) {
    ss_overview_copy w;
    ss_overview_work(ss, &w);
    ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
    if (strcmp(w.name, ss->ov_base_cfg.name) != 0) ge.name = w.name;
    if (ss->ov_icon_mode == SS_IMG_SET) ge.icon_data_uri = ss->ov_icon_uri;
    else if (ss->ov_icon_mode == SS_IMG_CLEAR) ge.icon_data_uri = "";
    if (ss->ov_banner_mode == SS_IMG_SET) ge.banner_data_uri = ss->ov_banner_uri;
    else if (ss->ov_banner_mode == SS_IMG_CLEAR) ge.banner_data_uri = "";
    if (w.system_channel_id != ss->ov_base_cfg.system_channel_id) {
      ge.has_system_channel = true;
      ge.system_channel_id = w.system_channel_id;
    }
    if (w.system_channel_flags != ss->ov_base_cfg.system_channel_flags) {
      ge.system_channel_flags = (int32_t)w.system_channel_flags;
    }
    if (w.default_notifications != ss->ov_base_cfg.default_notifications) {
      ge.default_notifications = (int8_t)w.default_notifications;
    }
    if (w.afk_channel_id != ss->ov_base_cfg.afk_channel_id) {
      ge.has_afk_channel = true;
      ge.afk_channel_id = w.afk_channel_id;
    }
    if (w.afk_timeout != ss->ov_base_cfg.afk_timeout) {
      ge.afk_timeout = (int32_t)w.afk_timeout;
    }
    ss->last_save_ms = ui->now_ms ? ui->now_ms : 1u;
    ss_submit(ui, ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err), &req, &err, "update the server");
    return;
  }
  if (ss->page == SS_SAFETY) {
    ccord_admin_guild_edit ge = CCORD_ADMIN_GUILD_EDIT_INIT;
    if (ss->safety_work.verification_level != ss->safety_base.verification_level) {
      ge.verification_level = (int8_t)ss->safety_work.verification_level;
    }
    if (ss->safety_work.explicit_filter != ss->safety_base.explicit_filter) {
      ge.explicit_content_filter = (int8_t)ss->safety_work.explicit_filter;
    }
    ss->last_save_ms = ui->now_ms ? ui->now_ms : 1u;
    ss_submit(ui, ccord_admin_edit_guild_ex(&req, ss->guild_id, &ge, nullptr, &err), &req, &err, "save safety settings");
    return;
  }
  ss_role_copy w = ss->work;
  ss_work_name(ss, &w);
  ccord_admin_role_edit e = CCORD_ADMIN_ROLE_EDIT_INIT;
  if (ss->role_sel != ss->guild_id && strcmp(w.name, ss->base.name) != 0) e.name = w.name;
  if (w.perms != ss->base.perms) e.permissions = (int64_t)w.perms;
  if (ss->role_sel != ss->guild_id) {
    if (w.color != ss->base.color) e.color = (int32_t)w.color;
    if (w.hoist != ss->base.hoist) e.hoist = w.hoist ? 1 : 0;
    if (w.mention != ss->base.mention) e.mentionable = w.mention ? 1 : 0;
  }
  ss->last_save_ms = ui->now_ms ? ui->now_ms : 1u;
  ss_submit(ui, ccord_admin_edit_role(&req, ss->guild_id, ss->role_sel, &e, nullptr, &err), &req, &err, "save the role");
}

void ss_reset(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->page == SS_OVERVIEW) {
    ss->ov_work_cfg = ss->ov_base_cfg;
    ss_free_ov_images(ss);
    ss_set_edit(&ss->name_edit, ss->ov_base_cfg.name);
  } else if (ss->page == SS_SAFETY) {
    ss->safety_work = ss->safety_base;
  } else if (ss->page == SS_ROLES) {
    const ccord_role *role = ccord_store_role(ui->d.store, ss->guild_id, ss->role_sel);
    if (role) ss_load_role(ss, role);
  }
  ss->focus = SSF_NONE;
  ui->dirty = true;
}

void ss_create_role(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_role *roles = ss_roles(ui, &n);
  ccord_id newest = 0;
  for (size_t i = 0; i < n; i++) {
    if (roles[i].id > newest) newest = roles[i].id;
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_role(&req, ss->guild_id, nullptr, nullptr, &err);
  if (st == CCORD_OK) {
    ss->await_role = true;
    ss->await_above = newest;
    ss->await_until_ms = ui->now_ms + SS_AWAIT_ROLE_MS;
  }
  ss_submit(ui, st, &req, &err, "create the role");
}

// Moves the selected role one place up or down by swapping positions with its neighbour.
void ss_move_role(ccord_ui *ui, bool up)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_role *roles = ss_roles(ui, &n);
  size_t i = SIZE_MAX;
  for (size_t k = 0; k < n; k++) {
    if (roles[k].id == ss->role_sel) i = k;
  }
  if (i == SIZE_MAX || ss->role_sel == ss->guild_id) return;
  if ((up && i == 0) || (!up && (i + 1 >= n || roles[i + 1].id == ss->guild_id))) return;
  const ccord_role *me = &roles[i], *nb = up ? &roles[i - 1] : &roles[i + 1];
  int32_t pos_me = nb->position, pos_nb = me->position;
  if (me->position == nb->position) {   // a tie: the mover goes strictly past its neighbour
    if (up) pos_me = nb->position + 1;
    else pos_nb = me->position + 1;
    if (!up) pos_me = me->position;
  }
  ccord_id ids[2] = {me->id, nb->id};
  int32_t pos[2] = {pos_me, pos_nb};
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_reorder_roles(&req, ss->guild_id, ids, pos, 2, nullptr, &err), &req, &err, "move the role");
}

void ss_toggle_member(ccord_ui *ui, ccord_id uid)
{
  struct ui_server_settings *ss = ui->ss;
  bool known = false;
  bool has = ss_member_has_role(ui, uid, ss->role_sel, &known);
  if (!known) return;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = has ? ccord_admin_remove_member_role(&req, ss->guild_id, uid, ss->role_sel, nullptr, &err)
                        : ccord_admin_add_member_role(&req, ss->guild_id, uid, ss->role_sel, nullptr, &err);
  ss_submit(ui, st, &req, &err, has ? "take the role away" : "give the role");
}

void ss_create_invite(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_id chan = ss_invite_channel(ui);
  if (chan == CCORD_ID_NONE) return;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_create_invite(&req, ss->guild_id, chan, ss_k_inv_ages[ss->inv_age_i], ss_k_inv_uses[ss->inv_uses_i], false, &err),
            &req, &err, "create the invite");
}

void ss_next_invite_channel(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  size_t n = ccord_store_channel_count(st, ss->guild_id);
  ccord_id cur = ss_invite_channel(ui), first = CCORD_ID_NONE, next = CCORD_ID_NONE;
  bool seen = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, i);
    if (!ch || ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_thread(ch->discord_type)) continue;
    if (first == CCORD_ID_NONE) first = ch->id;
    if (seen && next == CCORD_ID_NONE) next = ch->id;
    if (ch->id == cur) seen = true;
  }
  ss->inv_channel = next != CCORD_ID_NONE ? next : first;
}

void ss_cycle_overview_channel(ccord_ui *ui, bool voice)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  ccord_id cur = voice ? ss->ov_work_cfg.afk_channel_id : ss->ov_work_cfg.system_channel_id;
  size_t n = ccord_store_channel_count(st, ss->guild_id);
  ccord_id first = CCORD_ID_NONE;
  bool seen_cur = (cur == CCORD_ID_NONE);
  ccord_id chosen = CCORD_ID_NONE;
  bool found = false;
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, ss->guild_id, i);
    if (!ch) continue;
    if (voice) {
      if (ch->kind != CCORD_CHANNEL_VOICE || ch->discord_type == CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE) continue;
    } else {
      if (ch->kind != CCORD_CHANNEL_TEXT || ccord_discord_type_is_thread(ch->discord_type)) continue;
    }
    if (first == CCORD_ID_NONE) first = ch->id;
    if (seen_cur && !found) {
      chosen = ch->id;
      found = true;
    }
    if (ch->id == cur) seen_cur = true;
  }
  ccord_id next = found ? chosen : (seen_cur ? CCORD_ID_NONE : first);
  if (voice) ss->ov_work_cfg.afk_channel_id = next;
  else ss->ov_work_cfg.system_channel_id = next;
}

void ss_set_incident_pause(ccord_ui *ui, bool pause_invites, uint64_t duration_s)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  int64_t inv_until = gd->invites_disabled_until_ms > ui->now_unix_ms ? gd->invites_disabled_until_ms : 0;
  int64_t dms_until = gd->dms_disabled_until_ms > ui->now_unix_ms ? gd->dms_disabled_until_ms : 0;
  int64_t target_until = duration_s == 0 ? 0 : ui->now_unix_ms + (int64_t)duration_s * 1000LL;
  if (pause_invites) inv_until = target_until;
  else dms_until = target_until;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_set_incident_actions(&req, ss->guild_id, inv_until, dms_until, &err), &req, &err,
            "update raid protection");
}

void ss_save_vanity_code(ccord_ui *ui, const char *code)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ss_submit(ui, ccord_admin_set_vanity_url(&req, ss->guild_id, code, nullptr, &err), &req, &err, "set the vanity URL");
}

void ss_cycle_audit_user(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ss_populate_audit_actors(ui);
  if (ss->audit_actor_count == 0) {
    ss->audit_user_id = CCORD_ID_NONE;
    return;
  }
  if (ss->audit_user_id == CCORD_ID_NONE) {
    ss->audit_user_id = ss->audit_actor_ids[0];
  } else {
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < ss->audit_actor_count; i++) {
      if (ss->audit_actor_ids[i] == ss->audit_user_id) { idx = i; break; }
    }
    ss->audit_user_id = (idx == SIZE_MAX || idx + 1u >= ss->audit_actor_count)
                          ? CCORD_ID_NONE
                          : ss->audit_actor_ids[idx + 1u];
  }
  ss->requested[SS_AUDIT] = false;
  ss_request(ui, SS_AUDIT, true);
}

void ss_cycle_audit_action(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  size_t n = 0;
  const ccord_audit_action_info *tbl = ccord_audit_action_table(&n);
  if (!tbl || n == 0) return;
  if (ss->audit_action == 0) {
    ss->audit_action = tbl[0].action;
  } else {
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
      if (tbl[i].action == ss->audit_action) { idx = i; break; }
    }
    ss->audit_action = (idx == SIZE_MAX || idx + 1u >= n) ? 0u : tbl[idx + 1u].action;
  }
  ss->requested[SS_AUDIT] = false;
  ss_request(ui, SS_AUDIT, true);
}

void ss_cycle_transfer_member(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_store *st = ui->d.store;
  ccord_id self = ccord_store_self_id(st);
  ccord_id cur = ss_transfer_target(ui), first = CCORD_ID_NONE, next = CCORD_ID_NONE;
  bool seen = false;
  size_t n = ccord_store_member_count(st, ss->guild_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_member *m = ccord_store_member_at(st, ss->guild_id, i);
    if (!m || m->user_id == self) continue;
    const ccord_user *u = ccord_store_user(st, m->user_id);
    if (u && u->is_bot) continue;
    if (first == CCORD_ID_NONE) first = m->user_id;
    if (seen && next == CCORD_ID_NONE) next = m->user_id;
    if (m->user_id == cur) seen = true;
  }
  ss->transfer_user_id = next != CCORD_ID_NONE ? next : first;
}

const char *ss_hit_code(const ss_hit *h)
{
  const char *colon = strchr(h->name, ':');
  return colon ? colon + 1 : "";
}

bool ss_is_automod_list_field(ss_field f)
{
  return f == SSF_AM_KEYWORDS || f == SSF_AM_REGEX || f == SSF_AM_ALLOW;
}

void ss_automod_note_overflow(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const char *msg = ss->focus == SSF_AM_REGEX
                      ? "Regex list is too long: keep at most 10 patterns of 260 characters."
                      : "List is too long: keep at most 128 entries of 60 characters.";
  (void)snprintf(ss->am_error, sizeof ss->am_error, "%s", msg);
  ui_notice(ui, "Couldn't add text: %s", msg);
}

void ss_after_edit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->focus >= SSF_AM_NAME && ss->focus <= SSF_AM_CUSTOM_MSG) {   // a corrected field clears its old error
    ss->am_error[0] = '\0';
  }
  if (ss->focus == SSF_ROLE_COLOR) {   // the hex box drives the working colour while it holds a valid colour
    const char *t = ccord_text_edit_cstr(&ss->color_edit);
    bool ok = false;
    uint32_t v = ss_color_hex_parse(t, &ok);
    if (ok) ss->work.color = v;
    else if (t[0] == '\0') ss->work.color = 0;
  }
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
  ui->dirty = true;
}

void ss_try_close(ccord_ui *ui)
{
  if (ss_dirty(ui->ss)) {
    ss_flash(ui, "You have unsaved changes. Save or reset them first.");
    return;
  }
  ui_server_settings_close(ui);
}

static const char *ss_file_basename(const char *path)
{
  const char *slash = strrchr(path, '/');
  const char *bslash = strrchr(path, '\\');
  const char *base = path;
  if (slash && slash + 1 > base) base = slash + 1;
  if (bslash && bslash + 1 > base) base = bslash + 1;
  return base;
}

static void ss_prefill_stem(ccord_text_edit *ed, const char *base, bool emoji_ident)
{
  if (ed->len > 0 || !base || base[0] == '\0') return;
  const char *dot = strrchr(base, '.');
  size_t len = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  char buf[40];
  size_t out = 0;
  for (size_t i = 0; i < len && out + 1u < sizeof buf; i++) {
    char c = base[i];
    if (emoji_ident) {
      bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      buf[out++] = ok ? c : '_';
    } else {
      buf[out++] = c;
    }
  }
  buf[out] = '\0';
  if (out >= 2u) ss_set_edit(ed, buf);
}

static bool ss_read_picked_file(ccord_ui *ui, size_t max_bytes, const char *max_label, char path_out[static 512],
                                uint8_t **raw_out, size_t *len_out)
{
  if (!ui->d.host.open_file_dialog) {
    ui_notice(ui, "File picker is not available");
    return false;
  }
  path_out[0] = '\0';
  if (!ui->d.host.open_file_dialog(ui->d.host.ud, path_out, 512u) || path_out[0] == '\0') return false;
  FILE *fp = fopen(path_out, "rb");
  if (!fp) {
    ui_notice(ui, "Cannot open file: %s", path_out);
    return false;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    ui_notice(ui, "Cannot read file size");
    return false;
  }
  long sz = ftell(fp);
  if (sz <= 0 || (unsigned long)sz > max_bytes || fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    ui_notice(ui, "File must be between 1 B and %s", max_label);
    return false;
  }
  uint8_t *raw = ccord_malloc((size_t)sz);
  if (!raw) {
    fclose(fp);
    return false;
  }
  size_t got = fread(raw, 1u, (size_t)sz, fp);
  fclose(fp);
  if (got != (size_t)sz) {
    ccord_free(raw);
    ui_notice(ui, "Failed to read file");
    return false;
  }
  *raw_out = raw;
  *len_out = got;
  return true;
}

static bool ss_is_apng(const uint8_t *buf, size_t len)
{
  if (len < 8u || memcmp(buf, "\x89PNG\r\n\x1a\n", 8u) != 0) return false;
  size_t pos = 8u;
  while (pos + 12u <= len) {
    uint32_t clen = ((uint32_t)buf[pos] << 24u) | ((uint32_t)buf[pos + 1u] << 16u) |
                    ((uint32_t)buf[pos + 2u] << 8u) | (uint32_t)buf[pos + 3u];
    const uint8_t *ctype = buf + pos + 4u;
    if (memcmp(ctype, "acTL", 4u) == 0) return true;
    if (memcmp(ctype, "IDAT", 4u) == 0 || memcmp(ctype, "IEND", 4u) == 0) return false;
    if ((size_t)clen > len - pos - 12u) return false;
    pos += 12u + (size_t)clen;
  }
  return false;
}

static const char *ss_sniff_sticker_file(const uint8_t *buf, size_t len, ccord_sticker_format *fmt_out)
{
  if (ss_is_apng(buf, len)) {
    *fmt_out = CCORD_STICKER_APNG;
    return "image/apng";
  }
  if (len >= 8u && memcmp(buf, "\x89PNG\r\n\x1a\n", 8u) == 0) {
    *fmt_out = CCORD_STICKER_PNG;
    return "image/png";
  }
  if (len >= 6u && (memcmp(buf, "GIF87a", 6u) == 0 || memcmp(buf, "GIF89a", 6u) == 0)) {
    *fmt_out = CCORD_STICKER_GIF;
    return "image/gif";
  }
  size_t i = 0;
  while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
  if (i < len && buf[i] == '{' && ccord_utf8_valid_text((const char *)buf, len)) {
    *fmt_out = CCORD_STICKER_LOTTIE;
    return "application/json";
  }
  return nullptr;
}

static const char *ss_sniff_audio_mime(const uint8_t *buf, size_t len)
{
  if (len >= 4u && memcmp(buf, "OggS", 4u) == 0) return "audio/ogg";
  if (len >= 3u && memcmp(buf, "ID3", 3u) == 0) return "audio/mpeg";
  if (len >= 2u && buf[0] == 0xFFu && (buf[1] & 0xE0u) == 0xE0u) return "audio/mpeg";
  return nullptr;
}

void ss_pick_emoji_file(ccord_ui *ui)
{
  char path[512];
  uint8_t *raw = nullptr;
  size_t got = 0;
  if (!ss_read_picked_file(ui, SS_IMAGE_MAX_BYTES, "48 KB", path, &raw, &got)) return;
  const char *mime = ss_sniff_image_mime(raw, got);
  if (!mime) {
    ccord_free(raw);
    ui_notice(ui, "Choose a PNG, JPEG, GIF, or WebP image");
    return;
  }
  char *uri = ss_encode_data_uri(mime, raw, got);
  ccord_free(raw);
  if (!uri) {
    ui_notice(ui, "Out of memory encoding emoji");
    return;
  }
  struct ui_server_settings *ss = ui->ss;
  ccord_free(ss->emoji_upload_uri);
  ss->emoji_upload_uri = uri;
  ss->emoji_upload_anim = (strcmp(mime, "image/gif") == 0);
  const char *base = ss_file_basename(path);
  (void)snprintf(ss->emoji_upload_file, sizeof ss->emoji_upload_file, "%.127s", base);
  ss_prefill_stem(&ss->emoji_new_name, base, true);
}

void ss_pick_sticker_file(ccord_ui *ui)
{
  char path[512];
  uint8_t *raw = nullptr;
  size_t got = 0;
  if (!ss_read_picked_file(ui, SS_STICKER_MAX_BYTES, "60 KB", path, &raw, &got)) return;
  ccord_sticker_format fmt = CCORD_STICKER_PNG;
  const char *mime = ss_sniff_sticker_file(raw, got, &fmt);
  if (!mime) {
    ccord_free(raw);
    ui_notice(ui, "Choose a PNG, APNG, GIF, or Lottie JSON file");
    return;
  }
  struct ui_server_settings *ss = ui->ss;
  ccord_free(ss->sticker_upload_bytes);
  ss->sticker_upload_bytes = raw;
  ss->sticker_upload_len = got;
  (void)snprintf(ss->sticker_upload_mime, sizeof ss->sticker_upload_mime, "%s", mime);
  ss->sticker_upload_fmt = fmt;
  const char *base = ss_file_basename(path);
  (void)snprintf(ss->sticker_upload_file, sizeof ss->sticker_upload_file, "%.127s", base);
  ss_prefill_stem(&ss->sticker_new_name, base, false);
}

void ss_pick_sound_file(ccord_ui *ui)
{
  char path[512];
  uint8_t *raw = nullptr;
  size_t got = 0;
  if (!ss_read_picked_file(ui, SS_IMAGE_MAX_BYTES, "48 KB", path, &raw, &got)) return;
  const char *mime = ss_sniff_audio_mime(raw, got);
  if (!mime) {
    ccord_free(raw);
    ui_notice(ui, "Choose an MP3 or OGG audio file");
    return;
  }
  char *uri = ss_encode_data_uri(mime, raw, got);
  ccord_free(raw);
  if (!uri) {
    ui_notice(ui, "Out of memory encoding sound");
    return;
  }
  struct ui_server_settings *ss = ui->ss;
  ccord_free(ss->sound_upload_uri);
  ss->sound_upload_uri = uri;
  const char *base = ss_file_basename(path);
  (void)snprintf(ss->sound_upload_file, sizeof ss->sound_upload_file, "%.127s", base);
  ss_prefill_stem(&ss->sound_new_name, base, false);
}

bool ss_admin_send(ccord_ui *ui, const ccord_admin_request *req)
{
  if (!ui->d.session) {
    ui_notice(ui, "Not connected");
    return false;
  }
  ccord_error err;
  if (ccord_session_admin(ui->d.session, req, &err) != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", ccord_admin_op_verb(req->op), err.message);
    return false;
  }
  return true;
}

void ss_create_emoji_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss->emoji_upload_uri) {
    ui_notice(ui, "Choose an image file first.");
    return;
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_emoji(&req, ss->guild_id, ccord_text_edit_cstr(&ss->emoji_new_name),
                                             ss->emoji_upload_uri, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't upload emoji: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ccord_free(ss->emoji_upload_uri);
  ss->emoji_upload_uri = nullptr;
  ss->emoji_upload_anim = false;
  ss->emoji_upload_file[0] = '\0';
  ccord_text_edit_clear(&ss->emoji_new_name);
  ss->focus = SSF_NONE;
}

void ss_create_sticker_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss->sticker_upload_bytes || ss->sticker_upload_len == 0) {
    ui_notice(ui, "Choose a PNG, APNG, GIF, or Lottie file first.");
    return;
  }
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_sticker(&req, ss->guild_id, ccord_text_edit_cstr(&ss->sticker_new_name),
                                               ccord_text_edit_cstr(&ss->sticker_new_desc),
                                               ccord_text_edit_cstr(&ss->sticker_new_tags), ss->sticker_upload_bytes,
                                               ss->sticker_upload_len, ss->sticker_upload_mime, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't upload sticker: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ccord_free(ss->sticker_upload_bytes);
  ss->sticker_upload_bytes = nullptr;
  ss->sticker_upload_len = 0;
  ss->sticker_upload_mime[0] = '\0';
  ss->sticker_upload_file[0] = '\0';
  ccord_text_edit_clear(&ss->sticker_new_name);
  ccord_text_edit_clear(&ss->sticker_new_tags);
  ccord_text_edit_clear(&ss->sticker_new_desc);
  ss->focus = SSF_NONE;
}

void ss_create_sound_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (!ss->sound_upload_uri) {
    ui_notice(ui, "Choose an MP3 or OGG audio file first.");
    return;
  }
  const char *em = ccord_text_edit_cstr(&ss->sound_new_emoji);
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_create_soundboard(&req, ss->guild_id, ccord_text_edit_cstr(&ss->sound_new_name),
                                                  ss->sound_upload_uri, (int16_t)ss->sound_new_vol_pct, CCORD_ID_NONE,
                                                  em[0] ? em : nullptr, nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't upload sound: %s", err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ccord_free(ss->sound_upload_uri);
  ss->sound_upload_uri = nullptr;
  ss->sound_upload_file[0] = '\0';
  ss->sound_new_vol_pct = 100u;
  ccord_text_edit_clear(&ss->sound_new_name);
  ccord_text_edit_clear(&ss->sound_new_emoji);
  ss->focus = SSF_NONE;
}

void ss_begin_expr_edit(ccord_ui *ui, ss_page p, ccord_id id)
{
  struct ui_server_settings *ss = ui->ss;
  if (p == SS_EMOJI) {
    const ccord_emoji *em = ccord_store_find_emoji(ui->d.store, id);
    if (!em || em->guild_id != ss->guild_id) return;
    ss_set_edit(&ss->expr_edit_name, em->name ? em->name : "");
  } else if (p == SS_STICKERS) {
    const ccord_sticker *st_it = ccord_store_find_sticker(ui->d.store, id);
    if (!st_it || st_it->guild_id != ss->guild_id) return;
    ss_set_edit(&ss->expr_edit_name, st_it->name ? st_it->name : "");
    ss_set_edit(&ss->expr_edit_tags, st_it->tags ? st_it->tags : "");
    ss_set_edit(&ss->expr_edit_desc, st_it->description ? st_it->description : "");
  } else if (p == SS_SOUNDBOARD) {
    const ccord_soundboard_sound *sb = ccord_store_find_soundboard(ui->d.store, id);
    if (!sb || sb->guild_id != ss->guild_id) return;
    ss_set_edit(&ss->expr_edit_name, sb->name ? sb->name : "");
    ss_set_edit(&ss->expr_edit_emoji, sb->emoji_name ? sb->emoji_name : "");
    ss->edit_sound_vol_pct = sb->volume_pct;
  } else {
    return;
  }
  ss->edit_expr_page = p;
  ss->edit_expr_id = id;
  ss->focus = SSF_EXPR_EDIT_NAME;
  ccord_text_edit_move(&ss->expr_edit_name, 2, false);
  ui->caret_on = true;
  ui->caret_epoch_ms = ui->now_ms;
}

void ss_save_expr_edit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  if (ss->edit_expr_id == CCORD_ID_NONE) return;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = CCORD_ERR_INVALID;
  const char *what = "save the expression";
  if (ss->edit_expr_page == SS_EMOJI) {
    what = "rename the emoji";
    st = ccord_admin_edit_emoji(&req, ss->guild_id, ss->edit_expr_id, ccord_text_edit_cstr(&ss->expr_edit_name),
                                nullptr, &err);
  } else if (ss->edit_expr_page == SS_STICKERS) {
    what = "update the sticker";
    ccord_admin_sticker_edit ed = {
      .name = ccord_text_edit_cstr(&ss->expr_edit_name),
      .description = ccord_text_edit_cstr(&ss->expr_edit_desc),
      .tags = ccord_text_edit_cstr(&ss->expr_edit_tags),
    };
    st = ccord_admin_edit_sticker(&req, ss->guild_id, ss->edit_expr_id, &ed, nullptr, &err);
  } else if (ss->edit_expr_page == SS_SOUNDBOARD) {
    what = "update the sound";
    const ccord_soundboard_sound *sb_old = ccord_store_find_soundboard(ui->d.store, ss->edit_expr_id);
    ccord_admin_soundboard_edit ed = CCORD_ADMIN_SOUNDBOARD_EDIT_INIT;
    ed.name = ccord_text_edit_cstr(&ss->expr_edit_name);
    ed.volume_pct = (int16_t)ss->edit_sound_vol_pct;
    ed.has_emoji = true;
    ed.emoji_id = sb_old ? sb_old->emoji_id : CCORD_ID_NONE;
    ed.emoji_name = ccord_text_edit_cstr(&ss->expr_edit_emoji);
    st = ccord_admin_edit_soundboard(&req, ss->guild_id, ss->edit_expr_id, &ed, nullptr, &err);
  }
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't %s: %s", what, err.message);
    return;
  }
  if (!ss_admin_send(ui, &req)) return;
  ss->edit_expr_id = CCORD_ID_NONE;
  ss->focus = SSF_NONE;
}

void ss_delete_expr_open(ccord_ui *ui, ss_page p, ccord_id id)
{
  struct ui_server_settings *ss = ui->ss;
  if (p == SS_EMOJI) {
    const ccord_emoji *em = ccord_store_find_emoji(ui->d.store, id);
    if (!em || em->guild_id != ss->guild_id) return;
    ui_confirm_open(ui, CCORD_ADMIN_DELETE_EMOJI, ss->guild_id, id, em->name ? em->name : "");
  } else if (p == SS_STICKERS) {
    const ccord_sticker *st_it = ccord_store_find_sticker(ui->d.store, id);
    if (!st_it || st_it->guild_id != ss->guild_id) return;
    ui_confirm_open(ui, CCORD_ADMIN_DELETE_STICKER, ss->guild_id, id, st_it->name ? st_it->name : "");
  } else if (p == SS_SOUNDBOARD) {
    const ccord_soundboard_sound *sb = ccord_store_find_soundboard(ui->d.store, id);
    if (!sb || sb->guild_id != ss->guild_id) return;
    ui_confirm_open(ui, CCORD_ADMIN_DELETE_SOUNDBOARD, ss->guild_id, id, sb->name ? sb->name : "");
  }
}

ccord_id ss_hit_second_id(const ss_hit *h)
{
  const char *c1 = strchr(h->name, ':');
  if (!c1) return CCORD_ID_NONE;
  const char *c2 = strchr(c1 + 1, ':');
  if (!c2) return CCORD_ID_NONE;
  ccord_id id = CCORD_ID_NONE;
  return ccord_id_parse(c2 + 1, strlen(c2 + 1), &id) ? id : CCORD_ID_NONE;
}

void ss_member_role_change(ccord_ui *ui, ccord_id uid, ccord_id rid, bool add)
{
  struct ui_server_settings *ss = ui->ss;
  if (uid == CCORD_ID_NONE || rid == CCORD_ID_NONE) return;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = add ? ccord_admin_add_member_role(&req, ss->guild_id, uid, rid, nullptr, &err)
                        : ccord_admin_remove_member_role(&req, ss->guild_id, uid, rid, nullptr, &err);
  ss_submit(ui, st, &req, &err, add ? "give the role" : "remove the role");
}

void ss_member_timeout_act(ccord_ui *ui, ccord_id uid)
{
  struct ui_server_settings *ss = ui->ss;
  const ccord_member *m = ccord_store_find_member(ui->d.store, ss->guild_id, uid);
  if (m && m->timeout_until_ms > ui->now_unix_ms) {
    ccord_admin_request req;
    ccord_error err;
    ss_submit(ui, ccord_admin_timeout(&req, ss->guild_id, uid, ui->now_unix_ms, 0, nullptr, &err),
              &req, &err, "remove the timeout");
    return;
  }
  ui_confirm_open(ui, CCORD_ADMIN_TIMEOUT, ss->guild_id, uid, ui_member_display_name(ui, ss->guild_id, uid));
}

void ss_prune_preview_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_preview_prune(&req, ss->guild_id, ss->prune_days,
                                              ss->prune_role_count > 0 ? ss->prune_roles : nullptr,
                                              ss->prune_role_count, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't preview prune: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) ss->prune_dirty = false;
}

void ss_prune_execute_submit(ccord_ui *ui)
{
  struct ui_server_settings *ss = ui->ss;
  const char *reason = ccord_text_edit_cstr(&ss->prune_reason);
  ccord_admin_request req;
  ccord_error err;
  ccord_status st = ccord_admin_execute_prune(&req, ss->guild_id, ss->prune_days, true,
                                              ss->prune_role_count > 0 ? ss->prune_roles : nullptr,
                                              ss->prune_role_count,
                                              reason && reason[0] ? reason : nullptr, &err);
  if (st != CCORD_OK) {
    ui_notice(ui, "Couldn't prune members: %s", err.message);
    return;
  }
  if (ss_admin_send(ui, &req)) {
    ss->prune_confirm = false;
    ss->prune_dirty = false;
  }
}

