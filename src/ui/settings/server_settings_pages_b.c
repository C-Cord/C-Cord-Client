// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "server_settings_internal.h"

// ---------------------------------------------------------------- Expressions (Emoji, Stickers, Soundboard)

static const char *ss_expr_modify_why(const ccord_ui *ui, ccord_id guild, ccord_id creator_id)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  if (!ccord_store_permissions_exact(ui->d.store, guild, self)) return nullptr;
  uint64_t perms = ss_self_perms(ui, guild);
  if (!ccord_perm_can_create_expression(perms)) return "You need Manage Expressions to edit or delete this.";
  if (creator_id != CCORD_ID_NONE && !ccord_perm_can_modify_expression(perms, self, creator_id)) {
    return "You need Manage Expressions permission to modify expressions created by others.";
  }
  return nullptr;
}

static void ss_slot_bar(ss_cx *c, float x, float *py, float w, const char *label, uint32_t used, uint32_t limit)
{
  ccord_ui *ui = c->ui;
  float y = *py;
  ss_label(c, label, x, y);
  char counts[48];
  uint32_t free_slots = used < limit ? limit - used : 0u;
  (void)snprintf(counts, sizeof counts, "%u / %u used  (%u free)", (unsigned)used, (unsigned)limit, (unsigned)free_slots);
  float cw = ss_text_width(c, counts, SS_CAP(c), false);
  (void)ss_text(c, counts, x + w - cw, y, SS_CAP(c), false,
                used >= limit ? COL(danger) : COL(text_secondary), 0.0f);
  y += 20.0f;
  ccord_rect bar = {x, y, w, 8.0f};
  ccord_draw_rrect(c->r, bar, SS_R_CTRL(c), COL(bg_tertiary));
  if (used > 0 && limit > 0) {
    float frac = fminf(1.0f, (float)used / (float)limit);
    ccord_draw_rrect(c->r, (ccord_rect){bar.x, bar.y, fmaxf(8.0f, bar.w * frac), bar.h}, SS_R_CTRL(c),
                     used >= limit ? COL(danger) : COL(brand));
  }
  *py = y + 8.0f + 16.0f;
}

static const char *ss_sticker_fmt_label(ccord_sticker_format fmt)
{
  switch (fmt) {
    case CCORD_STICKER_PNG: return "PNG";
    case CCORD_STICKER_APNG: return "APNG";
    case CCORD_STICKER_LOTTIE: return "LOTTIE";
    case CCORD_STICKER_GIF: return "GIF";
  }
  return "PNG";
}

static const uint8_t k_sound_vols[4] = {25u, 50u, 75u, 100u};

void ss_draw_emoji(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Emoji", y);
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0,
            "refresh");
  y += 56.0f;

  size_t n = ccord_store_guild_emoji_count(ui->d.store, ss->guild_id);
  uint32_t static_used = 0u, anim_used = 0u;
  for (size_t i = 0; i < n; i++) {
    const ccord_emoji *em = ccord_store_guild_emoji_at(ui->d.store, ss->guild_id, i);
    if (!em) continue;
    if (em->animated) anim_used++;
    else static_used++;
  }
  uint32_t limit = ccord_guild_emoji_slot_limit(gd->premium_tier);
  ss_slot_bar(c, x, &y, w, "STATIC EMOJI SLOTS", static_used, limit);
  ss_slot_bar(c, x, &y, w, "ANIMATED EMOJI SLOTS", anim_used, limit);
  y += 6.0f;

  ss_label(c, "UPLOAD EMOJI", x, y);
  y += 24.0f;
  ss_button(c, (ccord_rect){x, y, 132.0f, 34.0f}, "Choose Image", BTN_SECONDARY, nullptr, A_EMOJI_PICK, 0, "emoji-pick");
  char file_info[192];
  if (ss->emoji_upload_uri) {
    (void)snprintf(file_info, sizeof file_info, "Selected: %s (%s)", ss->emoji_upload_file,
                   ss->emoji_upload_anim ? "Animated GIF" : "Static");
  } else {
    (void)snprintf(file_info, sizeof file_info, "PNG, JPEG, GIF, or WebP up to 48 KB");
  }
  (void)ss_text(c, file_info, x + 144.0f, y + 8.0f, M(font_size_small), false, COL(text_secondary), w - 144.0f);
  y += 34.0f + 12.0f;

  float name_w = fminf(w - 150.0f, 340.0f);
  ss_field_box(c, (ccord_rect){x, y, name_w, 36.0f}, &ss->emoji_new_name, SSF_EMOJI_NEW_NAME, "emoji_name (2-32 chars)",
               nullptr, "emoji-new-name");
  const char *up_why = nullptr;
  if (!ss->emoji_upload_uri) up_why = "Choose an image file first.";
  else if ((ss->emoji_upload_anim ? anim_used : static_used) >= limit) {
    up_why = "This server has reached its emoji slot limit.";
  }
  ss_button(c, (ccord_rect){x + name_w + 12.0f, y + 1.0f, 136.0f, 34.0f}, "Upload Emoji", BTN_PRIMARY, up_why,
            A_EMOJI_CREATE, 0, "emoji-upload");
  y += 36.0f + 28.0f;

  uis_divider(ui, c->r, x, y, w);
  y += 24.0f;

  ss_label(c, "SERVER EMOJI", x, y);
  y += 24.0f;
  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 36.0f}, &ss->emoji_search, SSF_EMOJI_SEARCH,
               "Search emoji by name or uploader", nullptr, "emoji-search");
  y += 36.0f + 20.0f;

  bool loaded = ccord_store_guild_emojis_admin_loaded(ui->d.store, ss->guild_id);
  if (ss_draw_list_state(c, SS_EMOJI, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  const char *needle = ccord_text_edit_cstr(&ss->emoji_search);
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_emoji *em = ccord_store_guild_emoji_at(ui->d.store, ss->guild_id, i);
    if (!em) continue;
    const char *em_name = em->name ? em->name : "";
    const char *up_name = (em->user_name && em->user_name[0]) ? em->user_name : "";
    if (!ss_contains_ci(em_name, needle) && !ss_contains_ci(up_name, needle)) continue;
    shown++;
    bool editing = (ss->edit_expr_page == SS_EMOJI && ss->edit_expr_id == em->id);
    ccord_rect row = {x, y, w, editing ? 60.0f : SS_ROW_H + 6.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;
    if (editing) {
      float fw = fminf(w - 180.0f, 320.0f);
      ss_field_box(c, (ccord_rect){row.x, row.y + 12.0f, fw, 36.0f}, &ss->expr_edit_name, SSF_EXPR_EDIT_NAME,
                   "emoji_name", nullptr, "emoji-edit-name");
      ss_button(c, (ccord_rect){row.x + fw + 10.0f, row.y + 13.0f, 74.0f, 34.0f}, "Save", BTN_PRIMARY, nullptr,
                A_EXPR_EDIT_SAVE, em->id, "emoji-edit-save");
      ss_button(c, (ccord_rect){row.x + fw + 92.0f, row.y + 13.0f, 78.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr,
                A_EXPR_EDIT_CANCEL, em->id, "emoji-edit-cancel");
    } else {
      char disp[CCORD_NAME_MAX_BYTES + 16];
      (void)snprintf(disp, sizeof disp, ":%s:%s", em_name, em->animated ? "  [GIF]" : "");
      (void)ss_text(c, disp, row.x + 8.0f, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 180.0f);
      char sub[CCORD_NAME_MAX_BYTES + 48];
      if (up_name[0]) (void)snprintf(sub, sizeof sub, "Uploaded by %s", up_name);
      else (void)snprintf(sub, sizeof sub, "Uploader unknown");
      (void)ss_text(c, sub, row.x + 8.0f, row.y + 8.0f + ui_line_h(ui, M(font_size_base)), M(font_size_small), false,
                    COL(text_muted), w - 180.0f);
      const char *mwhy = ss_expr_modify_why(ui, ss->guild_id, em->user_id);
      char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN];
      ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + (row.h - 30.0f) * 0.5f, 72.0f, 30.0f}, "Edit",
                BTN_SECONDARY, mwhy, A_EMOJI_EDIT, em->id,
                NAME(nm1, "emoji-edit:%llu", (unsigned long long)em->id));
      ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + (row.h - 30.0f) * 0.5f, 76.0f, 30.0f}, "Delete",
                BTN_DANGER, mwhy, A_EMOJI_DELETE, em->id,
                NAME(nm2, "emoji-delete:%llu", (unsigned long long)em->id));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "No custom emoji uploaded yet." : "No emoji match your search.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

void ss_draw_stickers(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Stickers", y);
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0,
            "refresh");
  y += 56.0f;

  size_t n = ccord_store_guild_sticker_count(ui->d.store, ss->guild_id);
  uint32_t limit = ccord_guild_sticker_slot_limit(gd->premium_tier);
  ss_slot_bar(c, x, &y, w, "STICKER SLOTS", (uint32_t)n, limit);
  y += 6.0f;

  ss_label(c, "UPLOAD STICKER", x, y);
  y += 24.0f;
  ss_button(c, (ccord_rect){x, y, 132.0f, 34.0f}, "Choose File", BTN_SECONDARY, nullptr, A_STICKER_PICK, 0,
            "sticker-pick");
  char file_info[192];
  if (ss->sticker_upload_bytes && ss->sticker_upload_len > 0) {
    (void)snprintf(file_info, sizeof file_info, "Selected: %s (%s, %zu B)", ss->sticker_upload_file,
                   ss_sticker_fmt_label(ss->sticker_upload_fmt), ss->sticker_upload_len);
  } else {
    (void)snprintf(file_info, sizeof file_info, "PNG, APNG, GIF, or Lottie JSON up to 60 KB");
  }
  (void)ss_text(c, file_info, x + 144.0f, y + 8.0f, M(font_size_small), false, COL(text_secondary), w - 144.0f);
  y += 34.0f + 12.0f;

  float half_w = fmaxf(140.0f, (w - 12.0f) * 0.5f);
  ss_field_box(c, (ccord_rect){x, y, half_w, 36.0f}, &ss->sticker_new_name, SSF_STICKER_NEW_NAME,
               "Sticker name (2-30 chars)", nullptr, "sticker-new-name");
  ss_field_box(c, (ccord_rect){x + half_w + 12.0f, y, half_w, 36.0f}, &ss->sticker_new_tags, SSF_STICKER_NEW_TAGS,
               "Related emoji / tags (required)", nullptr, "sticker-new-tags");
  y += 36.0f + 10.0f;
  float desc_w = fminf(w - 156.0f, 460.0f);
  ss_field_box(c, (ccord_rect){x, y, desc_w, 36.0f}, &ss->sticker_new_desc, SSF_STICKER_NEW_DESC,
               "Description (optional)", nullptr, "sticker-new-desc");
  const char *up_why = nullptr;
  if (!ss->sticker_upload_bytes || ss->sticker_upload_len == 0) up_why = "Choose a PNG, APNG, GIF, or Lottie file first.";
  else if ((uint32_t)n >= limit) up_why = "This server has reached its sticker slot limit.";
  ss_button(c, (ccord_rect){x + desc_w + 12.0f, y + 1.0f, 142.0f, 34.0f}, "Upload Sticker", BTN_PRIMARY, up_why,
            A_STICKER_CREATE, 0, "sticker-upload");
  y += 36.0f + 28.0f;

  uis_divider(ui, c->r, x, y, w);
  y += 24.0f;

  ss_label(c, "SERVER STICKERS", x, y);
  y += 24.0f;
  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 36.0f}, &ss->sticker_search, SSF_STICKER_SEARCH,
               "Search stickers by name, tags, or uploader", nullptr, "sticker-search");
  y += 36.0f + 20.0f;

  bool loaded = ccord_store_guild_stickers_admin_loaded(ui->d.store, ss->guild_id);
  if (ss_draw_list_state(c, SS_STICKERS, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  const char *needle = ccord_text_edit_cstr(&ss->sticker_search);
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_sticker *st_it = ccord_store_guild_sticker_at(ui->d.store, ss->guild_id, i);
    if (!st_it) continue;
    const char *sname = st_it->name ? st_it->name : "";
    const char *stags = st_it->tags ? st_it->tags : "";
    const char *sdesc = st_it->description ? st_it->description : "";
    const char *up_name = (st_it->user_name && st_it->user_name[0]) ? st_it->user_name : "";
    if (!ss_contains_ci(sname, needle) && !ss_contains_ci(stags, needle) && !ss_contains_ci(sdesc, needle) &&
        !ss_contains_ci(up_name, needle)) {
      continue;
    }
    shown++;
    bool editing = (ss->edit_expr_page == SS_STICKERS && ss->edit_expr_id == st_it->id);
    ccord_rect row = {x, y, w, editing ? 104.0f : SS_ROW_H + 10.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;
    if (editing) {
      float ew = fmaxf(120.0f, (w - 12.0f) * 0.5f);
      ss_field_box(c, (ccord_rect){row.x, row.y + 8.0f, ew, 36.0f}, &ss->expr_edit_name, SSF_EXPR_EDIT_NAME,
                   "Sticker name", nullptr, "sticker-edit-name");
      ss_field_box(c, (ccord_rect){row.x + ew + 12.0f, row.y + 8.0f, ew, 36.0f}, &ss->expr_edit_tags,
                   SSF_EXPR_EDIT_TAGS, "Tags", nullptr, "sticker-edit-tags");
      float dw = fminf(w - 180.0f, 400.0f);
      ss_field_box(c, (ccord_rect){row.x, row.y + 52.0f, dw, 36.0f}, &ss->expr_edit_desc, SSF_EXPR_EDIT_DESC,
                   "Description", nullptr, "sticker-edit-desc");
      ss_button(c, (ccord_rect){row.x + dw + 10.0f, row.y + 53.0f, 74.0f, 34.0f}, "Save", BTN_PRIMARY, nullptr,
                A_EXPR_EDIT_SAVE, st_it->id, "sticker-edit-save");
      ss_button(c, (ccord_rect){row.x + dw + 92.0f, row.y + 53.0f, 78.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr,
                A_EXPR_EDIT_CANCEL, st_it->id, "sticker-edit-cancel");
    } else {
      char disp[CCORD_NAME_MAX_BYTES + 24];
      (void)snprintf(disp, sizeof disp, "%s  [%s]", sname, ss_sticker_fmt_label(st_it->format));
      (void)ss_text(c, disp, row.x + 8.0f, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 180.0f);
      char sub[320];
      if (up_name[0] && sdesc[0]) {
        (void)snprintf(sub, sizeof sub, "Tags: %s  -  %s  -  Uploaded by %s", stags, sdesc, up_name);
      } else if (up_name[0]) {
        (void)snprintf(sub, sizeof sub, "Tags: %s  -  Uploaded by %s", stags, up_name);
      } else {
        (void)snprintf(sub, sizeof sub, "Tags: %s", stags);
      }
      (void)ss_text(c, sub, row.x + 8.0f, row.y + 8.0f + ui_line_h(ui, M(font_size_base)), M(font_size_small), false,
                    COL(text_muted), w - 180.0f);
      const char *mwhy = ss_expr_modify_why(ui, ss->guild_id, st_it->user_id);
      char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN];
      ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + (row.h - 30.0f) * 0.5f, 72.0f, 30.0f}, "Edit",
                BTN_SECONDARY, mwhy, A_STICKER_EDIT, st_it->id,
                NAME(nm1, "sticker-edit:%llu", (unsigned long long)st_it->id));
      ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + (row.h - 30.0f) * 0.5f, 76.0f, 30.0f}, "Delete",
                BTN_DANGER, mwhy, A_STICKER_DELETE, st_it->id,
                NAME(nm2, "sticker-delete:%llu", (unsigned long long)st_it->id));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "No custom stickers uploaded yet." : "No stickers match your search.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

void ss_draw_soundboard(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;
  float x = g->col_x, w = g->col_w, y = 56.0f - ss->scroll;
  ss_title(c, g, "Soundboard", y);
  ss_button(c, (ccord_rect){x + w - 100.0f, y - 2.0f, 100.0f, 32.0f}, "Refresh", BTN_SECONDARY, nullptr, A_REFRESH, 0,
            "refresh");
  y += 56.0f;

  size_t n = ccord_store_guild_soundboard_count(ui->d.store, ss->guild_id);
  uint32_t limit = ccord_guild_soundboard_slot_limit(gd->premium_tier);
  ss_slot_bar(c, x, &y, w, "SOUNDBOARD SLOTS", (uint32_t)n, limit);
  y += 6.0f;

  ss_label(c, "UPLOAD SOUND", x, y);
  y += 24.0f;
  ss_button(c, (ccord_rect){x, y, 132.0f, 34.0f}, "Choose Audio", BTN_SECONDARY, nullptr, A_SOUND_PICK, 0, "sound-pick");
  char file_info[192];
  if (ss->sound_upload_uri) {
    (void)snprintf(file_info, sizeof file_info, "Selected: %s", ss->sound_upload_file);
  } else {
    (void)snprintf(file_info, sizeof file_info, "MP3 or OGG up to 48 KB");
  }
  (void)ss_text(c, file_info, x + 144.0f, y + 8.0f, M(font_size_small), false, COL(text_secondary), w - 144.0f);
  y += 34.0f + 12.0f;

  float half_w = fmaxf(140.0f, (w - 12.0f) * 0.5f);
  ss_field_box(c, (ccord_rect){x, y, half_w, 36.0f}, &ss->sound_new_name, SSF_SOUND_NEW_NAME,
               "Sound name (2-32 chars)", nullptr, "sound-new-name");
  ss_field_box(c, (ccord_rect){x + half_w + 12.0f, y, half_w, 36.0f}, &ss->sound_new_emoji, SSF_SOUND_NEW_EMOJI,
               "Emoji (optional)", nullptr, "sound-new-emoji");
  y += 36.0f + 12.0f;

  (void)ss_text(c, "Volume:", x, y + 4.0f, M(font_size_small), true, COL(text_secondary), 0.0f);
  float cx = x + 64.0f;
  for (size_t vi = 0; vi < 4u; vi++) {
    char vlbl[16], vnm[SS_NAME_LEN];
    (void)snprintf(vlbl, sizeof vlbl, "%u%%", (unsigned)k_sound_vols[vi]);
    ss_chip(c, (ccord_rect){cx, y + 2.0f, 56.0f, 28.0f}, vlbl, ss->sound_new_vol_pct == k_sound_vols[vi],
            A_SOUND_NEW_VOL, k_sound_vols[vi], NAME(vnm, "sound-new-vol:%u", (unsigned)k_sound_vols[vi]));
    cx += 64.0f;
  }
  const char *up_why = nullptr;
  if (!ss->sound_upload_uri) up_why = "Choose an MP3 or OGG audio file first.";
  else if ((uint32_t)n >= limit) up_why = "This server has reached its soundboard slot limit.";
  ss_button(c, (ccord_rect){x + w - 138.0f, y - 1.0f, 138.0f, 34.0f}, "Upload Sound", BTN_PRIMARY, up_why,
            A_SOUND_CREATE, 0, "sound-upload");
  y += 34.0f + 28.0f;

  uis_divider(ui, c->r, x, y, w);
  y += 24.0f;

  ss_label(c, "SERVER SOUNDS", x, y);
  y += 24.0f;
  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 36.0f}, &ss->sound_search, SSF_SOUND_SEARCH,
               "Search sounds by name or uploader", nullptr, "sound-search");
  y += 36.0f + 20.0f;

  bool loaded = ccord_store_guild_soundboard_admin_loaded(ui->d.store, ss->guild_id);
  if (ss_draw_list_state(c, SS_SOUNDBOARD, loaded, x, &y, w)) {
    ss_end_content(ss, y);
    return;
  }
  const char *needle = ccord_text_edit_cstr(&ss->sound_search);
  size_t shown = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_soundboard_sound *sb = ccord_store_guild_soundboard_at(ui->d.store, ss->guild_id, i);
    if (!sb) continue;
    const char *sname = sb->name ? sb->name : "";
    const char *semoji = sb->emoji_name ? sb->emoji_name : "";
    const char *up_name = (sb->user_name && sb->user_name[0]) ? sb->user_name : "";
    if (!ss_contains_ci(sname, needle) && !ss_contains_ci(semoji, needle) && !ss_contains_ci(up_name, needle)) continue;
    shown++;
    bool editing = (ss->edit_expr_page == SS_SOUNDBOARD && ss->edit_expr_id == sb->id);
    ccord_rect row = {x, y, w, editing ? 104.0f : SS_ROW_H + 8.0f};
    y += row.h;
    if (!ss_visible(c, row)) continue;
    if (editing) {
      float ew = fmaxf(120.0f, (w - 12.0f) * 0.5f);
      ss_field_box(c, (ccord_rect){row.x, row.y + 8.0f, ew, 36.0f}, &ss->expr_edit_name, SSF_EXPR_EDIT_NAME,
                   "Sound name", nullptr, "sound-edit-name");
      ss_field_box(c, (ccord_rect){row.x + ew + 12.0f, row.y + 8.0f, ew, 36.0f}, &ss->expr_edit_emoji,
                   SSF_EXPR_EDIT_EMOJI, "Emoji", nullptr, "sound-edit-emoji");
      float vx = row.x;
      for (size_t vi = 0; vi < 4u; vi++) {
        char vlbl[16], vnm[SS_NAME_LEN];
        (void)snprintf(vlbl, sizeof vlbl, "%u%%", (unsigned)k_sound_vols[vi]);
        ss_chip(c, (ccord_rect){vx, row.y + 56.0f, 54.0f, 28.0f}, vlbl, ss->edit_sound_vol_pct == k_sound_vols[vi],
                A_EXPR_EDIT_VOL, k_sound_vols[vi], NAME(vnm, "sound-edit-vol:%u", (unsigned)k_sound_vols[vi]));
        vx += 60.0f;
      }
      ss_button(c, (ccord_rect){row.x + w - 162.0f, row.y + 53.0f, 74.0f, 34.0f}, "Save", BTN_PRIMARY, nullptr,
                A_EXPR_EDIT_SAVE, sb->id, "sound-edit-save");
      ss_button(c, (ccord_rect){row.x + w - 80.0f, row.y + 53.0f, 80.0f, 34.0f}, "Cancel", BTN_SECONDARY, nullptr,
                A_EXPR_EDIT_CANCEL, sb->id, "sound-edit-cancel");
    } else {
      char disp[CCORD_NAME_MAX_BYTES + CCORD_EMOJI_MAX_BYTES + 16];
      if (semoji[0]) (void)snprintf(disp, sizeof disp, "%s  %s", semoji, sname);
      else (void)snprintf(disp, sizeof disp, "%s", sname);
      (void)ss_text(c, disp, row.x + 8.0f, row.y + 6.0f, M(font_size_base), true, COL(text_header), w - 180.0f);
      char sub[CCORD_NAME_MAX_BYTES + 64];
      if (up_name[0]) {
        (void)snprintf(sub, sizeof sub, "Volume: %u%%  -  Uploaded by %s", (unsigned)sb->volume_pct, up_name);
      } else {
        (void)snprintf(sub, sizeof sub, "Volume: %u%%", (unsigned)sb->volume_pct);
      }
      (void)ss_text(c, sub, row.x + 8.0f, row.y + 8.0f + ui_line_h(ui, M(font_size_base)), M(font_size_small), false,
                    COL(text_muted), w - 180.0f);
      const char *mwhy = ss_expr_modify_why(ui, ss->guild_id, sb->user_id);
      char nm1[SS_NAME_LEN], nm2[SS_NAME_LEN];
      ss_button(c, (ccord_rect){row.x + w - 156.0f, row.y + (row.h - 30.0f) * 0.5f, 72.0f, 30.0f}, "Edit",
                BTN_SECONDARY, mwhy, A_SOUND_EDIT, sb->id,
                NAME(nm1, "sound-edit:%llu", (unsigned long long)sb->id));
      ss_button(c, (ccord_rect){row.x + w - 76.0f, row.y + (row.h - 30.0f) * 0.5f, 76.0f, 30.0f}, "Delete",
                BTN_DANGER, mwhy, A_SOUND_DELETE, sb->id,
                NAME(nm2, "sound-delete:%llu", (unsigned long long)sb->id));
    }
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }
  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, n == 0 ? "No custom soundboard sounds uploaded yet." : "No sounds match your search.", x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);
}

// ---------------------------------------------------------------- Members, Prune & Mod View

static void ss_format_date_ms(int64_t ms, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  if (ms <= 0) {
    (void)snprintf(out, cap, "Unknown");
    return;
  }
  static const char *const k_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  time_t sec = (time_t)(ms / 1000LL);
  struct tm tm_val;
  if (ccord_gmtime(sec, &tm_val) && tm_val.tm_mon >= 0 && tm_val.tm_mon < 12) {
    (void)snprintf(out, cap, "%s %d, %d", k_months[tm_val.tm_mon], tm_val.tm_mday, tm_val.tm_year + 1900);
  } else {
    (void)snprintf(out, cap, "Jan 1, 2015");
  }
}

static void ss_format_age_span(int64_t age_ms, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  if (age_ms <= 0) {
    (void)snprintf(out, cap, "just now");
    return;
  }
  int64_t s = age_ms / 1000LL;
  if (s < 3600LL) {
    (void)snprintf(out, cap, "%lldm", (long long)((s + 59LL) / 60LL));
  } else if (s < 86400LL) {
    (void)snprintf(out, cap, "%lldh", (long long)(s / 3600LL));
  } else {
    int64_t days = s / 86400LL;
    if (days >= 365LL) {
      (void)snprintf(out, cap, "%lldy %lldd", (long long)(days / 365LL), (long long)(days % 365LL));
    } else {
      (void)snprintf(out, cap, "%lldd", (long long)days);
    }
  }
}

static const char *ss_member_act_why(const ccord_ui *ui, ccord_id guild, ccord_id target, ccord_perm_action act)
{
  ccord_id self = ccord_store_self_id(ui->d.store);
  ccord_perm_verdict v = ccord_store_can_act_on(ui->d.store, act, guild, self, target);
  return v == CCORD_PERM_ALLOWED ? nullptr : ccord_perm_verdict_text(v, act);
}

bool ss_id_in_array(const ccord_id *arr, size_t count, ccord_id id)
{
  for (size_t i = 0; i < count; i++) {
    if (arr[i] == id) return true;
  }
  return false;
}

void ss_toggle_id_array(ccord_id *arr, size_t *count, size_t cap, ccord_id id)
{
  for (size_t i = 0; i < *count; i++) {
    if (arr[i] == id) {
      for (size_t k = i; k + 1u < *count; k++) arr[k] = arr[k + 1u];
      (*count)--;
      return;
    }
  }
  if (*count < cap) arr[(*count)++] = id;
}

bool ss_prune_matches_state(const struct ui_server_settings *ss, bool has_pr, const ccord_prune_info *pr)
{
  return has_pr && pr && pr->has_result && !ss->prune_dirty && pr->days == ss->prune_days;
}

static void draw_modview_drawer(ss_cx *c, ccord_rect box)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  const ccord_member *m = ccord_store_find_member(ui->d.store, ss->guild_id, ss->modview_uid);
  if (!gd || !m) return;
  const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);

  ss->modview_rect = box;
  uis_modal_frame(c->ui, c->r, box);

  ss_cx dc = *c;
  dc.clip = box;
  ccord_push_clip(c->r, box);

  float x = box.x + 14.0f, w = box.w - 28.0f;
  float y = box.y + 12.0f;
  (void)ss_text(&dc, "MOD VIEW", x, y + 6.0f, SS_CAP(c), true, COL(text_muted), w - 76.0f);
  ss_button(&dc, (ccord_rect){x + w - 68.0f, y, 68.0f, 26.0f}, "Close", BTN_SECONDARY, nullptr, A_MODVIEW_CLOSE, 0,
            "modview-close");
  y += 32.0f;
  uis_divider(ui, c->r, box.x, y, box.w);
  y += 1.0f;

  ccord_rect scroll_area = {box.x, y, box.w, box.h - (y - box.y)};
  dc.clip = scroll_area;
  ccord_pop_clip(c->r);
  ccord_push_clip(c->r, scroll_area);

  float cy = scroll_area.y + 12.0f - ss->modview_scroll;
  const char *dname = ui_member_display_name(ui, ss->guild_id, m->user_id);
  ui_draw_avatar(ui, c->r, u, x + 20.0f, cy + 20.0f, 38.0f, (ccord_color){0, 0, 0, 0}, false);
  (void)ss_text(&dc, dname, x + 48.0f, cy + 2.0f, M(font_size_base), true, COL(text_header), w - 48.0f);
  char handle[CCORD_NAME_MAX_BYTES + 32];
  char idbuf[24];
  (void)ccord_id_format(m->user_id, idbuf);
  if (u && u->name && u->name[0]) {
    (void)snprintf(handle, sizeof handle, "@%s%s  (%s)", u->name, u->is_bot ? " [BOT]" : "", idbuf);
  } else {
    (void)snprintf(handle, sizeof handle, "ID: %s", idbuf);
  }
  (void)ss_text(&dc, handle, x + 48.0f, cy + 22.0f, SS_CAP(c), false, COL(text_muted), w - 48.0f);
  cy += 50.0f;

  // Quick Mod Actions inside drawer
  bool timed_out = m->timeout_until_ms > ui->now_unix_ms;
  const char *to_why = ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_TIMEOUT);
  const char *ki_why = ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_KICK);
  const char *ba_why = ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_BAN);
  float bw3 = (w - 12.0f) / 3.0f;
  ss_button(&dc, (ccord_rect){x, cy, bw3, 28.0f}, timed_out ? "Clear T/O" : "Timeout", BTN_SECONDARY, to_why,
            A_MEM_TIMEOUT, m->user_id, "modview-timeout");
  ss_button(&dc, (ccord_rect){x + bw3 + 6.0f, cy, bw3, 28.0f}, "Kick", BTN_SECONDARY, ki_why, A_MEM_KICK, m->user_id,
            "modview-kick");
  ss_button(&dc, (ccord_rect){x + (bw3 + 6.0f) * 2.0f, cy, bw3, 28.0f}, "Ban", BTN_DANGER, ba_why, A_MEM_BAN, m->user_id,
            "modview-ban");
  cy += 28.0f + 16.0f;

  // Member & Account Age / Join Info
  ss_label(&dc, "MEMBER INFORMATION", x, cy);
  cy += 20.0f;
  int64_t acct_ms = 1420070400000LL + (int64_t)(m->user_id >> 22);
  char acct_date[32], acct_age[32], acct_line[96];
  ss_format_date_ms(acct_ms, acct_date, sizeof acct_date);
  ss_format_age_span(ui->now_unix_ms - acct_ms, acct_age, sizeof acct_age);
  (void)snprintf(acct_line, sizeof acct_line, "Discord Account: %s (%s ago)", acct_date, acct_age);
  (void)ss_text(&dc, acct_line, x, cy, M(font_size_small), false, COL(text_normal), w);
  cy += 20.0f;

  char join_line[96];
  if (m->joined_at_ms > 0) {
    char jdate[32], jage[32];
    ss_format_date_ms(m->joined_at_ms, jdate, sizeof jdate);
    ss_format_age_span(ui->now_unix_ms - m->joined_at_ms, jage, sizeof jage);
    (void)snprintf(join_line, sizeof join_line, "Joined Server: %s (%s ago)", jdate, jage);
  } else {
    (void)snprintf(join_line, sizeof join_line, "Joined Server: Not reported");
  }
  (void)ss_text(&dc, join_line, x, cy, M(font_size_small), false, COL(text_normal), w);
  cy += 20.0f;

  const char *join_method = "Invite link / Server Discovery";
  if (m->user_id == gd->owner_id) join_method = "Server Creator / Owner";
  else if (u && u->is_bot) join_method = "OAuth2 Bot Integration";
  else if (gd->vanity_url_code[0] != '\0') join_method = "Invite or Vanity URL";
  char jmeth_line[96];
  (void)snprintf(jmeth_line, sizeof jmeth_line, "Join Method: %s", join_method);
  (void)ss_text(&dc, jmeth_line, x, cy, M(font_size_small), false, COL(text_secondary), w);
  cy += 20.0f;

  if (timed_out) {
    char rem[32], tline[80];
    ss_format_remaining(m->timeout_until_ms - ui->now_unix_ms, rem, sizeof rem);
    (void)snprintf(tline, sizeof tline, "Timed Out: %s remaining", rem);
    (void)ss_text(&dc, tline, x, cy, M(font_size_small), true, COL(danger), w);
    cy += 20.0f;
  }
  cy += 10.0f;

  // Permissions breakdown
  ss_label(&dc, "PERMISSIONS BREAKDOWN", x, cy);
  cy += 20.0f;
  uint64_t mp = ccord_store_permissions(ui->d.store, ss->guild_id, CCORD_ID_NONE, m->user_id, 0);
  if (m->user_id == gd->owner_id) {
    (void)ss_text(&dc, "Server Owner (all permissions)", x, cy, M(font_size_small), true, COL(brand), w);
    cy += 20.0f;
  } else if (ccord_perm_has(mp, CCORD_PERM_ADMINISTRATOR)) {
    (void)ss_text(&dc, "Administrator (all permissions)", x, cy, M(font_size_small), true, COL(brand), w);
    cy += 20.0f;
  } else {
    size_t np = 0, granted = 0;
    const ccord_perm_info *ptbl = ccord_perm_table(&np);
    static const uint64_t k_key_perms[] = {
      CCORD_PERM_MANAGE_GUILD, CCORD_PERM_MANAGE_ROLES, CCORD_PERM_MANAGE_CHANNELS,
      CCORD_PERM_KICK_MEMBERS, CCORD_PERM_BAN_MEMBERS, CCORD_PERM_MODERATE_MEMBERS,
      CCORD_PERM_MANAGE_MESSAGES, CCORD_PERM_MANAGE_WEBHOOKS, CCORD_PERM_MANAGE_EXPRESSIONS,
      CCORD_PERM_VIEW_AUDIT_LOG
    };
    size_t key_shown = 0;
    for (size_t k = 0; k < sizeof k_key_perms / sizeof k_key_perms[0]; k++) {
      if (ccord_perm_has(mp, k_key_perms[k])) {
        const char *plbl = ccord_perm_label(k_key_perms[k]);
        if (plbl) {
          char pbuf[64];
          (void)snprintf(pbuf, sizeof pbuf, "+ %s", plbl);
          (void)ss_text(&dc, pbuf, x, cy, M(font_size_small), false, COL(text_header), w);
          cy += 18.0f;
          key_shown++;
        }
      }
    }
    for (size_t k = 0; ptbl && k < np; k++) {
      if (ccord_perm_has(mp, ptbl[k].bit)) granted++;
    }
    char sum_p[80];
    (void)snprintf(sum_p, sizeof sum_p, "%zu of %zu server permissions granted%s", granted, np,
                   key_shown == 0 ? " (no elevated mod permissions)" : "");
    (void)ss_text(&dc, sum_p, x, cy, SS_CAP(c), false, COL(text_muted), w);
    cy += 20.0f;
  }
  cy += 10.0f;

  // Recent Audit Log entries involving this member
  ss_label(&dc, "RECENT MODERATION / AUDIT ACTIVITY", x, cy);
  cy += 20.0f;
  if (!ccord_perm_has(ss_self_perms(ui, ss->guild_id), CCORD_PERM_VIEW_AUDIT_LOG)) {
    (void)ss_text(&dc, "View Audit Log permission required.", x, cy, SS_CAP(c), false, COL(text_muted), w);
    cy += 20.0f;
  } else if (!ccord_store_audit_loaded(ui->d.store, ss->guild_id)) {
    (void)ss_text(&dc, "Loading audit activity...", x, cy, SS_CAP(c), false, COL(text_muted), w);
    cy += 20.0f;
  } else {
    size_t na = 0, hits = 0;
    const ccord_audit_entry *log = ccord_store_audit(ui->d.store, ss->guild_id, &na);
    for (size_t i = 0; log && i < na && hits < 8u; i++) {
      if (log[i].user_id != m->user_id && log[i].target_id != m->user_id) continue;
      hits++;
      const char *actor = log[i].user_name && log[i].user_name[0] ? log[i].user_name : "Someone";
      char aline[160];
      if (log[i].target_name && log[i].target_name[0]) {
        (void)snprintf(aline, sizeof aline, "%s %s %s", actor, ccord_audit_action_label(log[i].action), log[i].target_name);
      } else {
        (void)snprintf(aline, sizeof aline, "%s %s", actor, ccord_audit_action_label(log[i].action));
      }
      (void)ss_text(&dc, aline, x, cy, SS_CAP(c), false, COL(text_normal), w);
      cy += 17.0f;
      if (log[i].reason && log[i].reason[0]) {
        char rline[160];
        (void)snprintf(rline, sizeof rline, "  Reason: %s", log[i].reason);
        (void)ss_text(&dc, rline, x, cy, SS_CAP(c), false, COL(text_muted), w);
        cy += 17.0f;
      }
    }
    if (hits == 0) {
      (void)ss_text(&dc, "No audit entries recorded for this member.", x, cy, SS_CAP(c), false, COL(text_muted), w);
      cy += 20.0f;
    }
  }

  ss->modview_content_h = (cy + ss->modview_scroll) - scroll_area.y + 16.0f;
  ccord_pop_clip(c->r);
}

static void draw_prune_panel(ss_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float y = *py;

  ccord_rect acc = {x, y, w, 4.0f};
  ccord_draw_rrect(c->r, acc, acc.h * 0.5f, COL(danger));
  y += 12.0f;
  (void)ss_text(c, "Prune Members", x, y, M(font_size_base), true, COL(text_header), w);
  y += 22.0f;
  (void)ss_text(c, "Removes offline members who have not logged into Discord within the selected inactivity window.",
                x, y, SS_CAP(c), false, COL(text_muted), w);
  y += 24.0f;

  ss_label(c, "LAST SEEN (INACTIVE AT LEAST)", x, y);
  y += 22.0f;
  static const uint8_t k_pdays[4] = {1u, 7u, 14u, 30u};
  static const char *const k_pday_lbls[4] = {"1 day", "7 days", "14 days", "30 days"};
  float cx = x;
  for (size_t i = 0; i < 4u; i++) {
    float cw = ss_text_width(c, k_pday_lbls[i], SS_CAP(c), false) + 24.0f;
    char nm[SS_NAME_LEN];
    ss_chip(c, (ccord_rect){cx, y, cw, 26.0f}, k_pday_lbls[i], ss->prune_days == k_pdays[i], A_PRUNE_DAYS, k_pdays[i],
            NAME(nm, "prune-days:%u", (unsigned)k_pdays[i]));
    cx += cw + 8.0f;
  }
  y += 26.0f + 14.0f;

  size_t nroles = 0;
  const ccord_role *roles = ss_roles(ui, &nroles);
  bool has_extra_roles = false;
  for (size_t i = 0; roles && i < nroles; i++) {
    if (roles[i].id != ss->guild_id && !roles[i].managed) { has_extra_roles = true; break; }
  }
  if (has_extra_roles) {
    ss_label(c, "ALSO INCLUDE MEMBERS WITH ROLES (OPTIONAL)", x, y);
    y += 22.0f;
    cx = x;
    for (size_t i = 0; i < nroles; i++) {
      const ccord_role *ro = &roles[i];
      if (ro->id == ss->guild_id || ro->managed) continue;
      bool sel = ss_id_in_array(ss->prune_roles, ss->prune_role_count, ro->id);
      if (!ss_can_prune_role(ui, ss->guild_id, ro) && !sel) continue;
      const char *rlbl = ro->name && ro->name[0] ? ro->name : "role";
      float cw = fminf(180.0f, ss_text_width(c, rlbl, SS_CAP(c), false) + 24.0f);
      if (cx + cw > x + w && cx > x) { cx = x; y += 30.0f; }
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, rlbl, sel, A_PRUNE_ROLE, ro->id,
              NAME(nm, "prune-role:%llu", (unsigned long long)ro->id));
      cx += cw + 8.0f;
    }
    y += 25.0f + 14.0f;
  }

  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 34.0f}, &ss->prune_reason, SSF_PRUNE_REASON,
               "Audit log reason (optional)", nullptr, "prune-reason");
  y += 34.0f + 12.0f;

  ccord_prune_info pr = {0};
  bool has_pr = ccord_store_prune_info(ui->d.store, ss->guild_id, &pr);
  bool pr_ok = ss_prune_matches_state(ss, has_pr, &pr);
  char pmsg[160];
  if (pr_ok) {
    if (pr.executed) {
      if (pr.pruned >= 0) {
        (void)snprintf(pmsg, sizeof pmsg, "Pruned %d inactive member%s (%u-day window).",
                       (int)pr.pruned, pr.pruned == 1 ? "" : "s", (unsigned)pr.days);
      } else {
        (void)snprintf(pmsg, sizeof pmsg, "Prune request accepted by Discord (%u-day window).", (unsigned)pr.days);
      }
    } else if (pr.pruned >= 0) {
      (void)snprintf(pmsg, sizeof pmsg, "Estimate: %d member%s will be pruned (%u-day inactivity).",
                     (int)pr.pruned, pr.pruned == 1 ? "" : "s", (unsigned)pr.days);
    } else {
      (void)snprintf(pmsg, sizeof pmsg, "Click Preview Count to ask Discord how many members match.");
    }
  } else {
    (void)snprintf(pmsg, sizeof pmsg, "Click Preview Count to ask Discord how many members match.");
  }
  (void)ss_text(c, pmsg, x, y + 6.0f, SS_CAP(c), true,
                pr_ok && !pr.executed && pr.pruned > 0 ? COL(danger) : COL(text_secondary), w);
  y += 28.0f;

  ss_button(c, (ccord_rect){x, y, 128.0f, 32.0f}, "Preview Count", BTN_SECONDARY, nullptr, A_PRUNE_PREVIEW, 0,
            "prune-preview");
  if (!ss->prune_confirm) {
    ss_button(c, (ccord_rect){x + 138.0f, y, 126.0f, 32.0f}, "Prune Members", BTN_DANGER, nullptr, A_PRUNE_EXEC, 0,
              "prune-exec");
  } else {
    ss_button(c, (ccord_rect){x + 138.0f, y, 132.0f, 32.0f}, "Confirm Prune", BTN_DANGER, nullptr, A_PRUNE_CONFIRM, 0,
              "prune-confirm");
    ss_button(c, (ccord_rect){x + 278.0f, y, 84.0f, 32.0f}, "Cancel", BTN_SECONDARY, nullptr, A_PRUNE_CANCEL, 0,
              "prune-cancel");
  }
  y += 32.0f + 20.0f;
  uis_divider(ui, c->r, x, y, w);
  *py = y + 20.0f;
}

static void ss_ensure_member_cache(const ccord_ui *ui, struct ui_server_settings *ss, const char *needle)
{
  uint64_t rev = ccord_store_revision(ui->d.store);
  if (ss->mem_cache_valid && ss->mem_cache_rev == rev && ss->mem_cache_guild == ss->guild_id &&
      ss->mem_cache_role_filter == ss->member_role_filter &&
      strncmp(ss->mem_cache_needle, needle, sizeof ss->mem_cache_needle) == 0) {
    return;
  }
  ss->mem_cache_count = 0;
  size_t total_members = ccord_store_member_count(ui->d.store, ss->guild_id);
  bool has_needle = needle && needle[0] != '\0';
  bool has_role_filter = ss->member_role_filter != CCORD_ID_NONE;
  for (size_t i = 0; i < total_members; i++) {
    const ccord_member *m = ccord_store_member_at(ui->d.store, ss->guild_id, i);
    if (!m) continue;
    if (has_needle) {
      const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);
      const char *dname = ui_member_display_name(ui, ss->guild_id, m->user_id);
      const char *uname = (u && u->name) ? u->name : "";
      char idbuf[24];
      (void)ccord_id_format(m->user_id, idbuf);
      if (!ss_contains_ci(dname, needle) && !ss_contains_ci(uname, needle) &&
          !ss_contains_ci(m->nick, needle) && !ss_contains_ci(idbuf, needle)) {
        continue;
      }
    }
    if (has_role_filter) {
      const ccord_id *mroles = nullptr;
      size_t nmroles = 0;
      bool roles_known = ccord_store_member_roles(ui->d.store, ss->guild_id, m->user_id, &mroles, &nmroles);
      if (!roles_known || !ss_id_in_array(mroles, nmroles, ss->member_role_filter)) continue;
    }
    if (ss->mem_cache_count == ss->mem_cache_cap) {
      size_t ncap = ss->mem_cache_cap ? ss->mem_cache_cap * 2u : 64u;
      size_t *nv = ccord_realloc(ss->mem_cache_idx, ncap * sizeof *nv);
      if (!nv) break;
      ss->mem_cache_idx = nv;
      ss->mem_cache_cap = ncap;
    }
    ss->mem_cache_idx[ss->mem_cache_count++] = i;
  }
  ss->mem_cache_rev = rev;
  ss->mem_cache_guild = ss->guild_id;
  ss->mem_cache_role_filter = ss->member_role_filter;
  (void)snprintf(ss->mem_cache_needle, sizeof ss->mem_cache_needle, "%s", needle ? needle : "");
  ss->mem_cache_valid = true;
}

static void draw_member_row(ss_cx *c, const ccord_guild *gd, const ccord_member *m, const ccord_role *roles,
                            size_t nroles, bool roles_known, const ccord_id *mroles, size_t nmroles,
                            bool picker_open, bool is_owner, bool can_manage_roles, ccord_id self_id, ccord_rect row)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  float w = row.w;
  const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);
  const char *dname = ui_member_display_name(ui, ss->guild_id, m->user_id);
  const char *uname = (u && u->name) ? u->name : "";
  char idbuf[24];
  (void)ccord_id_format(m->user_id, idbuf);

  if (ss->modview_uid == m->user_id) {
    uis_list_row_fill(ui, c->r, (ccord_rect){row.x - 4.0f, row.y + 2.0f, row.w + 8.0f, row.h - 4.0f},
                      (uis_state){.selected = true}, "ssmodrow", m->user_id);
  }

  ui_draw_avatar(ui, c->r, u, row.x + 18.0f, row.y + 24.0f, 30.0f, (ccord_color){0, 0, 0, 0}, false);

  // Top line: display name + badges + joined_at + account age
  char title_line[CCORD_NAME_MAX_BYTES * 2 + 64];
  bool timed_out = m->timeout_until_ms > ui->now_unix_ms;
  (void)snprintf(title_line, sizeof title_line, "%s%s%s%s", dname,
                 m->user_id == gd->owner_id ? " [Owner]" : "",
                 (u && u->is_bot) ? " [BOT]" : "",
                 timed_out ? " [Timed Out]" : "");
  float act_bar_w = is_owner && m->user_id != self_id ? 298.0f : 236.0f;
  (void)ss_text(c, title_line, row.x + 42.0f, row.y + 6.0f, M(font_size_base), true,
                timed_out ? COL(danger) : COL(text_header), fmaxf(60.0f, w - 46.0f - act_bar_w));

  char joined_str[32], acct_str[32], meta_line[160];
  if (m->joined_at_ms > 0) ss_format_date_ms(m->joined_at_ms, joined_str, sizeof joined_str);
  else (void)snprintf(joined_str, sizeof joined_str, "-");
  int64_t acct_ms = 1420070400000LL + (int64_t)(m->user_id >> 22);
  ss_format_date_ms(acct_ms, acct_str, sizeof acct_str);
  (void)snprintf(meta_line, sizeof meta_line, "@%s  -  Joined: %s  -  Discord: %s",
                 uname[0] ? uname : idbuf, joined_str, acct_str);
  (void)ss_text(c, meta_line, row.x + 42.0f, row.y + 25.0f, SS_CAP(c), false, COL(text_muted),
                fmaxf(60.0f, w - 46.0f - act_bar_w));

  // Action buttons on right side of row
  float bx = row.x + w;
  char nm[SS_NAME_LEN];
  if (is_owner && m->user_id != self_id) {
    bx -= 58.0f;
    const char *xf_why = (u && u->is_bot) ? "Bots cannot own a server." : nullptr;
    ss_button(c, (ccord_rect){bx, row.y + 8.0f, 58.0f, 26.0f}, "Owner", BTN_SECONDARY, xf_why, A_MEM_TRANSFER,
              m->user_id, NAME(nm, "member-transfer:%llu", (unsigned long long)m->user_id));
    bx -= 4.0f;
  }
  bx -= 48.0f;
  ss_button(c, (ccord_rect){bx, row.y + 8.0f, 48.0f, 26.0f}, "Ban", BTN_DANGER,
            ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_BAN), A_MEM_BAN, m->user_id,
            NAME(nm, "member-ban:%llu", (unsigned long long)m->user_id));
  bx -= 52.0f;
  ss_button(c, (ccord_rect){bx, row.y + 8.0f, 48.0f, 26.0f}, "Kick", BTN_SECONDARY,
            ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_KICK), A_MEM_KICK, m->user_id,
            NAME(nm, "member-kick:%llu", (unsigned long long)m->user_id));
  bx -= 68.0f;
  ss_button(c, (ccord_rect){bx, row.y + 8.0f, 64.0f, 26.0f}, timed_out ? "Unmute" : "Timeout", BTN_SECONDARY,
            ss_member_act_why(ui, ss->guild_id, m->user_id, CCORD_PERM_ACT_TIMEOUT), A_MEM_TIMEOUT, m->user_id,
            NAME(nm, "member-timeout:%llu", (unsigned long long)m->user_id));
  bx -= 68.0f;
  ss_button(c, (ccord_rect){bx, row.y + 8.0f, 64.0f, 26.0f}, "Mod View", BTN_SECONDARY, nullptr, A_MEM_MODVIEW,
            m->user_id, NAME(nm, "member-modview:%llu", (unsigned long long)m->user_id));

  // Inline role chips (+/-)
  float rx = row.x + 42.0f, ry = row.y + 44.0f;
  if (roles_known) {
    for (size_t mi = 0; mi < nmroles; mi++) {
      const ccord_role *ro = ccord_store_role(ui->d.store, ss->guild_id, mroles[mi]);
      if (!ro || ro->id == ss->guild_id) continue;
      ccord_perm_verdict rv = ss_role_verdict(ui, ss->guild_id, ro->id, true);
      const char *rwhy = rv == CCORD_PERM_ALLOWED ? nullptr : ccord_perm_verdict_text(rv, CCORD_PERM_ACT_EDIT_ROLES);
      char clbl[CCORD_NAME_MAX_BYTES + 8];
      (void)snprintf(clbl, sizeof clbl, "%s%s", ro->name ? ro->name : "role", rwhy == nullptr ? " x" : "");
      // The pill is a uis_chip; its inset holds the role colour dot on the left edge.
      float cw = fminf(150.0f, ss_text_width(c, clbl, SS_CAP(c), false) + 16.0f) + 12.0f;
      if (rx + cw > row.x + w - 58.0f) break;
      ccord_rect pill = {rx, ry, cw, 21.0f};
      bool phov = ss_hit_add(c, pill, A_MEM_ROLE_DEL, m->user_id, rwhy,
                             NAME(nm, "member-role-del:%llu:%llu", (unsigned long long)m->user_id,
                                  (unsigned long long)ro->id)) && rwhy == nullptr;
      ccord_color pcol = ro->color ? ss_rgb(ro->color) : COL(text_secondary);
      uis_chip(ui, c->r, pill, clbl, (uis_state){.hover = phov, .disabled = rwhy != nullptr}, "ssmrole", ro->id);
      ccord_draw_circle(c->r, pill.x + 5.0f, pill.y + pill.h * 0.5f, 2.5f, pcol);
      rx += cw + 5.0f;
    }
    if (can_manage_roles && roles && nroles > 1u) {
      ccord_rect add_rc = {rx, ry, 52.0f, 21.0f};
      ss_button(c, add_rc, picker_open ? "- Role" : "+ Role", BTN_SECONDARY, nullptr, A_MEM_ROLE_PICK, m->user_id,
                NAME(nm, "member-role-pick:%llu", (unsigned long long)m->user_id));
    }
  } else {
    (void)ss_text(c, "Roles not loaded", rx, ry + 3.0f, SS_CAP(c), false, COL(text_muted), 140.0f);
  }

  if (picker_open) {
    float px = row.x + 42.0f, py_pick = row.y + 72.0f;
    size_t offered = 0;
    for (size_t ri = 0; ri < nroles; ri++) {
      const ccord_role *ro = &roles[ri];
      if (ro->id == ss->guild_id || ss_id_in_array(mroles, nmroles, ro->id)) continue;
      ccord_perm_verdict rv = ss_role_verdict(ui, ss->guild_id, ro->id, true);
      const char *rwhy = rv == CCORD_PERM_ALLOWED ? nullptr : ccord_perm_verdict_text(rv, CCORD_PERM_ACT_EDIT_ROLES);
      char albl[CCORD_NAME_MAX_BYTES + 8];
      (void)snprintf(albl, sizeof albl, "+ %s", ro->name ? ro->name : "role");
      float cw = fminf(150.0f, ss_text_width(c, albl, SS_CAP(c), false) + 18.0f);
      if (px + cw > row.x + w) break;
      ss_button(c, (ccord_rect){px, py_pick, cw, 22.0f}, albl, BTN_SECONDARY, rwhy, A_MEM_ROLE_ADD, m->user_id,
                NAME(nm, "member-role-add:%llu:%llu", (unsigned long long)m->user_id, (unsigned long long)ro->id));
      px += cw + 6.0f;
      offered++;
    }
    if (offered == 0) {
      (void)ss_text(c, "Member already holds every role.", px, py_pick + 3.0f, SS_CAP(c), false,
                    COL(text_muted), w - 48.0f);
    }
  }

  uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
}

void ss_draw_members(ss_cx *c, const ss_geom *g)
{
  ccord_ui *ui = c->ui;
  struct ui_server_settings *ss = c->ss;
  const ccord_guild *gd = ccord_store_guild(ui->d.store, ss->guild_id);
  if (!gd) return;

  bool has_drawer = ss->modview_uid != CCORD_ID_NONE && ccord_store_find_member(ui->d.store, ss->guild_id, ss->modview_uid);
  float avail_w = ui->win_w - g->col_x - 24.0f;
  float drawer_w = has_drawer ? fminf(340.0f, fmaxf(250.0f, avail_w * 0.36f)) : 0.0f;
  float x = g->col_x;
  float w = has_drawer ? fmaxf(280.0f, avail_w - drawer_w - 20.0f) : g->col_w;
  float y = 56.0f - ss->scroll;

  uint64_t self_perms = ss_self_perms(ui, ss->guild_id);
  ccord_id self_id = ccord_store_self_id(ui->d.store);
  bool can_prune = ccord_perm_has(self_perms, CCORD_PERM_KICK_MEMBERS);
  const char *prune_why = can_prune ? nullptr : "Pruning members requires the Kick Members permission.";

  ss_title(c, g, "Server Members", y);
  ss_button(c, (ccord_rect){x + w - 104.0f, y - 2.0f, 104.0f, 32.0f}, ss->prune_open ? "Close Prune" : "Prune",
            ss->prune_open ? BTN_SECONDARY : BTN_DANGER, prune_why, A_PRUNE_TOGGLE, 0, "prune-toggle");
  y += 52.0f;

  if (ss->prune_open && can_prune) {
    draw_prune_panel(c, x, w, &y);
  }

  size_t total_members = ccord_store_member_count(ui->d.store, ss->guild_id);
  char count_lbl[64];
  (void)snprintf(count_lbl, sizeof count_lbl, "%zu MEMBERS LOADED", total_members);
  ss_label(c, count_lbl, x, y);
  y += 22.0f;

  ss_field_box(c, (ccord_rect){x, y, fminf(w, 420.0f), 36.0f}, &ss->member_search, SSF_MEMBER_SEARCH,
               "Search by display name, username, or ID", nullptr, "member-search");
  y += 36.0f + 12.0f;

  // Role filter chips (chip widths cached per store revision + font size to avoid 250x text measures/frame)
  size_t nroles = 0;
  const ccord_role *roles = ss_roles(ui, &nroles);
  if (roles && nroles > 1u) {
    uint64_t rev = ccord_store_revision(ui->d.store);
    float cap_fs = SS_CAP(c);
    if (ss->mem_role_w_count != nroles || ss->mem_role_w_rev != rev || ss->mem_role_w_fs != cap_fs) {
      if (nroles > ss->mem_role_w_cap) {
        float *nw = ccord_realloc(ss->mem_role_w, nroles * sizeof *nw);
        if (nw) {
          ss->mem_role_w = nw;
          ss->mem_role_w_cap = nroles;
        }
      }
      if (ss->mem_role_w && ss->mem_role_w_cap >= nroles) {
        for (size_t ri = 0; ri < nroles; ri++) {
          if (roles[ri].id == ss->guild_id) {
            ss->mem_role_w[ri] = 0.0f;
          } else {
            const char *rname = roles[ri].name && roles[ri].name[0] ? roles[ri].name : "role";
            ss->mem_role_w[ri] = fminf(160.0f, ss_text_width(c, rname, cap_fs, false) + 22.0f);
          }
        }
        ss->mem_role_w_rev = rev;
        ss->mem_role_w_fs = cap_fs;
        ss->mem_role_w_count = nroles;
      }
    }
    float cx = x;
    float all_w = ss_text_width(c, "All Roles", cap_fs, false) + 22.0f;
    ss_chip(c, (ccord_rect){cx, y, all_w, 25.0f}, "All Roles", ss->member_role_filter == CCORD_ID_NONE,
            A_MEM_ROLE_FILTER, CCORD_ID_NONE, "member-filter:0");
    cx += all_w + 6.0f;
    for (size_t ri = 0; ri < nroles; ri++) {
      if (roles[ri].id == ss->guild_id) continue;
      const char *rname = roles[ri].name && roles[ri].name[0] ? roles[ri].name : "role";
      float cw = (ss->mem_role_w && ri < ss->mem_role_w_count)
                   ? ss->mem_role_w[ri]
                   : fminf(160.0f, ss_text_width(c, rname, cap_fs, false) + 22.0f);
      if (cx + cw > x + w && cx > x) { cx = x; y += 29.0f; }
      char nm[SS_NAME_LEN];
      ss_chip(c, (ccord_rect){cx, y, cw, 25.0f}, rname, ss->member_role_filter == roles[ri].id, A_MEM_ROLE_FILTER,
              roles[ri].id, NAME(nm, "member-filter:%llu", (unsigned long long)roles[ri].id));
      cx += cw + 6.0f;
    }
    y += 25.0f + 16.0f;
  }

  const char *needle = ccord_text_edit_cstr(&ss->member_search);
  ss_ensure_member_cache(ui, ss, needle);
  size_t shown = ss->mem_cache_count;
  bool is_owner = ss_is_owner(ui, ss->guild_id);
  bool can_manage_roles = ccord_perm_has(self_perms, CCORD_PERM_MANAGE_ROLES);

  if (ss->member_role_picker_uid == CCORD_ID_NONE) {
    // Fixed 72px rows: O(1) jump to the first visible row so off-screen members do zero store/chip work.
    float list_y0 = y;
    size_t first_k = 0;
    if (list_y0 < c->clip.y) {
      float skip = floorf((c->clip.y - list_y0) / 72.0f);
      if (skip > 0.0f) first_k = (size_t)skip;
    }
    if (first_k > shown) first_k = shown;
    for (size_t k = first_k; k < shown; k++) {
      float row_y = list_y0 + (float)k * 72.0f;
      if (row_y >= c->clip.y + c->clip.h) break;
      ccord_rect row = {x, row_y, w, 72.0f};
      if (!ss_visible(c, row)) continue;
      const ccord_member *m = ccord_store_member_at(ui->d.store, ss->guild_id, ss->mem_cache_idx[k]);
      if (!m) continue;
      const ccord_id *mroles = nullptr;
      size_t nmroles = 0;
      bool roles_known = ccord_store_member_roles(ui->d.store, ss->guild_id, m->user_id, &mroles, &nmroles);
      draw_member_row(c, gd, m, roles, nroles, roles_known, mroles, nmroles, false, is_owner, can_manage_roles,
                      self_id, row);
    }
    y = list_y0 + (float)shown * 72.0f;
  } else {
    for (size_t k = 0; k < shown; k++) {
      const ccord_member *m = ccord_store_member_at(ui->d.store, ss->guild_id, ss->mem_cache_idx[k]);
      if (!m) continue;
      const ccord_id *mroles = nullptr;
      size_t nmroles = 0;
      bool roles_known = false;
      bool picker_open = false;
      if (ss->member_role_picker_uid == m->user_id && roles && nroles > 1u) {
        roles_known = ccord_store_member_roles(ui->d.store, ss->guild_id, m->user_id, &mroles, &nmroles);
        picker_open = roles_known;
      }
      float row_h = picker_open ? 108.0f : 72.0f;
      ccord_rect row = {x, y, w, row_h};
      y += row_h;
      if (!ss_visible(c, row)) continue;
      if (!picker_open) {
        roles_known = ccord_store_member_roles(ui->d.store, ss->guild_id, m->user_id, &mroles, &nmroles);
      }
      draw_member_row(c, gd, m, roles, nroles, roles_known, mroles, nmroles, picker_open, is_owner, can_manage_roles,
                      self_id, row);
    }
  }

  ss->rows = shown;
  if (shown == 0) {
    ss_message(c, total_members == 0 ? "No members loaded yet." : "No members match the current search or role filter.",
               x, y, w);
    y += 40.0f;
  }
  ss_end_content(ss, y);

  if (has_drawer) {
    ccord_rect dbox = {x + w + 16.0f, 88.0f, drawer_w, fmaxf(180.0f, ui->win_h - 108.0f)};
    draw_modview_drawer(c, dbox);
  } else {
    ss->modview_rect = (ccord_rect){0};
  }
}

