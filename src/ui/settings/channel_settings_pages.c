// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "channel_settings_internal.h"

// ---------------------------------------------------------------- drawing primitives

static bool cs_visible(const cs_cx *c, ccord_rect r)
{
  return r.y + r.h >= c->clip.y && r.y <= c->clip.y + c->clip.h;
}

bool cs_add_hit(cs_cx *c, ccord_rect r, cs_act act, uint64_t arg, const char *why, const char *name)
{
  if (c->cs->nhits >= CS_HIT_MAX) return false;
  float x0 = fmaxf(r.x, c->clip.x), y0 = fmaxf(r.y, c->clip.y);
  float x1 = fminf(r.x + r.w, c->clip.x + c->clip.w), y1 = fminf(r.y + r.h, c->clip.y + c->clip.h);
  if (x1 <= x0 || y1 <= y0) return false;
  size_t idx = c->cs->nhits++;
  cs_hit *h = &c->cs->hits[idx];
  h->r = (ccord_rect){x0, y0, x1 - x0, y1 - y0};
  h->act = act;
  h->arg = arg;
  h->why = why;
  (void)snprintf(h->name, sizeof h->name, "%s", name ? name : "");
  return c->cs->hover == (int)idx;
}

float cs_text(cs_cx *c, const char *s, float x, float y, float size, bool bold, ccord_color col, float max_w)
{
  ccord_text_style st = ui_style(c->ui, bold ? CCORD_FONT_BOLD : CCORD_FONT_REGULAR, size, col);
  if (s && s[0] != '\0') {
    if (max_w > 0.0f) ccord_text_draw_ellipsis(c->r, s, strlen(s), x, y, max_w, &st);
    else ccord_text_draw(c->r, s, strlen(s), x, y, &st, 0.0f);
  }
  return ui_line_h(c->ui, size);
}

static void cs_label(cs_cx *c, const char *s, float x, float y)
{
  (void)cs_text(c, s, x, y, uis_tokens_of(c->ui).fs_overline, true, c->ui->theme->c.text_muted, 0.0f);
}

// Buttons snap to the nearest token height (28 / 32 / 38): below 30 is small, below 35 medium, else large.
static uis_btn_size cs_btn_size(float h)
{
  return h < 30.0f ? UIS_BTN_S_SM : (h < 35.0f ? UIS_BTN_S_MD : UIS_BTN_S_LG);
}

void cs_button(cs_cx *c, ccord_rect r, const char *label, uis_btn_kind kind, const char *why, cs_act act,
                      uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  uis_btn_size size = cs_btn_size(r.h);
  r.h = uis_button_height(size);
  if (!cs_visible(c, r)) return;
  bool hov = cs_add_hit(c, r, act, arg, why, name);
  uis_button(ui, c->r, r, label, kind, size, (uis_state){.hover = hov, .disabled = why != nullptr}, "csbtn",
             ui_name_id(name));
}

static void cs_chip(cs_cx *c, ccord_rect r, const char *label, bool on, const char *why, cs_act act, uint64_t arg,
                    const char *name)
{
  ccord_ui *ui = c->ui;
  r.h = UIS_H_BTN_SM;
  if (!cs_visible(c, r)) return;
  bool hov = cs_add_hit(c, r, act, arg, why, name);
  uis_chip(ui, c->r, r, label, (uis_state){.hover = hov, .selected = on, .disabled = why != nullptr}, "cschip", ui_name_id(name));
}

static void cs_toggle(cs_cx *c, ccord_rect r, bool on, const char *why, cs_act act, uint64_t arg, const char *name)
{
  ccord_ui *ui = c->ui;
  r = uis_toggle_rect(r);
  if (!cs_visible(c, r)) return;
  bool hov = cs_add_hit(c, r, act, arg, why, name);
  uis_toggle(ui, c->r, r, on, (uis_state){.hover = hov, .disabled = why != nullptr}, "cssw", ui_name_id(name));
}

// A text input (uis_input). A field whose edit does not exist yet is drawn empty.
static void cs_field_box(cs_cx *c, ccord_rect r, cs_field f, const char *placeholder, const char *why, const char *name)
{
  ccord_ui *ui = c->ui;
  r.h = UIS_H_INPUT;
  if (!cs_visible(c, r)) return;
  bool focused = c->cs->focus == f && !why;
  (void)cs_add_hit(c, r, CA_FIELD, (uint64_t)f, why, name);
  ccord_text_edit blank = {0};
  ccord_text_edit *ed = cs_edit_for(c->cs, f);
  (void)uis_input(ui, c->r, r, ed ? ed : &blank, placeholder ? placeholder : "",
                  (uis_state){.focused = focused, .disabled = why != nullptr}, false, "csfocus", (uint64_t)f);
}

// ---------------------------------------------------------------- Overview page

void cs_draw_cs_overview(cs_cx *c, const ccord_channel *ch, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_channel_settings *cs = c->cs;
  float y = *py;

  (void)cs_text(c, "Overview", x, y, CS_TITLE(c), true, COL(text_header), w);
  y += 34.0f;

  cs_label(c, "CHANNEL NAME", x, y);
  y += 20.0f;
  cs_field_box(c, (ccord_rect){x, y, w, 38.0f}, CSF_NAME, "new-channel", nullptr, "field:name");
  y += 52.0f;

  bool has_topic = ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type);
  if (has_topic) {
    cs_label(c, ccord_discord_type_is_forum(ch->discord_type) ? "POST GUIDELINES" : "CHANNEL TOPIC", x, y);
    y += 20.0f;
    cs_field_box(c, (ccord_rect){x, y, w, 38.0f}, CSF_TOPIC, "Let everyone know how to use this channel!", nullptr,
                 "field:topic");
    y += 52.0f;
  }

  if (ch->kind != CCORD_CHANNEL_CATEGORY) {
    cs_label(c, "SLOWMODE", x, y);
    y += 20.0f;
    float cx = x;
    for (size_t i = 0; i < SLOWMODE_COUNT; i++) {
      float cw = 54.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 34.0f;
      }
      char nm[CS_NAME_LEN];
      cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, cs_k_slowmode_labels[i], cs->ov_work.slowmode_s == cs_k_slowmodes[i],
              nullptr, CA_SLOWMODE, cs_k_slowmodes[i], NAME(nm, "slowmode:%u", (unsigned)cs_k_slowmodes[i]));
      cx += cw + 6.0f;
    }
    y += 44.0f;
  }

  if (ch->kind != CCORD_CHANNEL_CATEGORY && !ccord_discord_type_is_thread(ch->discord_type)) {
    ccord_rect nr = {x, y, w, 48.0f};
    (void)cs_text(c, "Age-Restricted Channel", x, y + 4.0f, uis_tokens_of(ui).fs_body, true, COL(text_header), w - 60.0f);
    (void)cs_text(c, "Users will need to confirm they are of over legal age to view the content in this channel.",
                  x, y + 24.0f, CS_CAP(c), false, COL(text_muted), w - 60.0f);
    cs_toggle(c, (ccord_rect){x + w - 44.0f, y + 10.0f, 44.0f, 24.0f}, cs->ov_work.nsfw, nullptr, CA_NSFW, 0, "nsfw");
    (void)nr;
    y += 60.0f;
  }

  if (ch->kind == CCORD_CHANNEL_VOICE) {
    cs_label(c, "BITRATE", x, y);
    y += 20.0f;
    float cx = x;
    for (size_t i = 0; i < BITRATE_COUNT; i++) {
      float cw = 78.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 34.0f;
      }
      char nm[CS_NAME_LEN];
      cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, cs_k_bitrate_labels[i], cs->ov_work.bitrate == cs_k_bitrates[i],
              nullptr, CA_BITRATE, cs_k_bitrates[i], NAME(nm, "bitrate:%u", (unsigned)cs_k_bitrates[i]));
      cx += cw + 6.0f;
    }
    y += 44.0f;

    cs_label(c, "USER LIMIT", x, y);
    y += 20.0f;
    cx = x;
    for (size_t i = 0; i < USER_LIMIT_COUNT; i++) {
      float cw = i == 0 ? 84.0f : 44.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 34.0f;
      }
      char nm[CS_NAME_LEN];
      cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, cs_k_user_limit_labels[i], cs->ov_work.user_limit == cs_k_user_limits[i],
              nullptr, CA_USER_LIMIT, cs_k_user_limits[i], NAME(nm, "user_limit:%u", (unsigned)cs_k_user_limits[i]));
      cx += cw + 6.0f;
    }
    if (cx + 66.0f > x + w) {
      cx = x;
      y += 34.0f;
    }
    cs_button(c, (ccord_rect){cx + (cx > x ? 6.0f : 0.0f), y, 28.0f, 28.0f}, "-", UIS_BTN_SECONDARY,
              cs->ov_work.user_limit == 0 ? "Already unlimited." : nullptr, CA_USER_LIMIT_STEP, 0, "user_limit:dec");
    cs_button(c, (ccord_rect){cx + (cx > x ? 38.0f : 32.0f), y, 28.0f, 28.0f}, "+", UIS_BTN_SECONDARY,
              cs->ov_work.user_limit >= CCORD_ADMIN_USER_LIMIT_MAX ? "Maximum user limit reached." : nullptr,
              CA_USER_LIMIT_STEP, 1, "user_limit:inc");
    y += 44.0f;
  }

  if (ch->kind == CCORD_CHANNEL_TEXT && !ccord_discord_type_is_thread(ch->discord_type)) {
    cs_label(c, "HIDE AFTER INACTIVITY", x, y);
    y += 20.0f;
    float cx = x;
    for (size_t i = 0; i < ARCHIVE_COUNT; i++) {
      float cw = 88.0f;
      if (cx > x && cx + cw > x + w) {
        cx = x;
        y += 34.0f;
      }
      char nm[CS_NAME_LEN];
      cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, cs_k_archive_labels[i], cs->ov_work.archive_min == cs_k_archives[i],
              nullptr, CA_ARCHIVE, cs_k_archives[i], NAME(nm, "archive:%u", (unsigned)cs_k_archives[i]));
      cx += cw + 8.0f;
    }
    y += 44.0f;
  }

  if (ccord_discord_type_is_forum(ch->discord_type)) {
    cs_label(c, "TAGS", x, y);
    y += 22.0f;
    cs->dbg_rows = cs->ov_work.tag_count;
    for (size_t i = 0; i < cs->ov_work.tag_count; i++) {
      const ccord_forum_tag *tg = &cs->ov_work.tags[i];
      ccord_rect rr = {x, y, w, 38.0f};
      if (cs_visible(c, rr)) {
        ccord_draw_rrect(c->r, rr, uis_tokens_of(ui).r_control, COL(bg_secondary));
        char line[128];
        if (tg->emoji_name[0] != '\0') (void)snprintf(line, sizeof line, "%s %s", tg->emoji_name, tg->name);
        else (void)snprintf(line, sizeof line, "%s", tg->name);
        (void)cs_text(c, line, x + 12.0f, y + 10.0f, uis_tokens_of(ui).fs_body, false, COL(text_header), w - 210.0f);
        char nm[CS_NAME_LEN];
        cs_chip(c, (ccord_rect){x + w - 186.0f, y + 5.0f, 96.0f, 28.0f},
                tg->moderated ? "Moderated" : "Anyone", tg->moderated, nullptr, CA_TAG_MOD, i,
                NAME(nm, "tag_mod:%zu", i));
        cs_button(c, (ccord_rect){x + w - 82.0f, y + 5.0f, 74.0f, 28.0f}, "Remove", UIS_BTN_DANGER,
                  nullptr, CA_TAG_DEL, i, NAME(nm, "tag_del:%zu", i));
      }
      y += 44.0f;
    }
    if (cs->ov_work.tag_count < CCORD_FORUM_MAX_TAGS) {
      if (w < 480.0f) {
        float ew = fminf(92.0f, w * 0.30f);
        float nw = fmaxf(w - ew - 8.0f, 80.0f);
        cs_field_box(c, (ccord_rect){x, y, nw, 34.0f}, CSF_TAG_NAME, "New tag name", nullptr, "field:tag_name");
        cs_field_box(c, (ccord_rect){x + nw + 8.0f, y, ew, 34.0f}, CSF_TAG_EMOJI, "Emoji", nullptr,
                     "field:tag_emoji");
        y += 40.0f;
        cs_chip(c, (ccord_rect){x, y, 100.0f, 28.0f}, "Moderated", cs->tag_new_mod, nullptr,
                CA_TAG_NEW_MOD, 0, "tag_new_mod");
        cs_button(c, (ccord_rect){x + 108.0f, y, 92.0f, 28.0f}, "Add Tag", UIS_BTN_PRIMARY,
                  nullptr, CA_TAG_ADD, 0, "tag_add");
        y += 40.0f;
      } else {
        cs_field_box(c, (ccord_rect){x, y, w * 0.45f, 34.0f}, CSF_TAG_NAME, "New tag name", nullptr, "field:tag_name");
        cs_field_box(c, (ccord_rect){x + w * 0.47f, y, w * 0.20f, 34.0f}, CSF_TAG_EMOJI, "Emoji", nullptr,
                     "field:tag_emoji");
        cs_chip(c, (ccord_rect){x + w * 0.69f, y + 3.0f, 92.0f, 28.0f}, "Moderated", cs->tag_new_mod, nullptr,
                CA_TAG_NEW_MOD, 0, "tag_new_mod");
        cs_button(c, (ccord_rect){x + w - 84.0f, y + 3.0f, 84.0f, 28.0f}, "Add Tag", UIS_BTN_PRIMARY,
                  nullptr, CA_TAG_ADD, 0, "tag_add");
        y += 46.0f;
      }
    }
  }

  *py = y;
}

// ---------------------------------------------------------------- Permissions page (tri-state overwrites)

static int cs_cmp_id(const void *a, const void *b)
{
  ccord_id ia = *(const ccord_id *)a;
  ccord_id ib = *(const ccord_id *)b;
  return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

static bool cs_id_in_sorted(const ccord_id *ids, size_t n, ccord_id target)
{
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (ids[mid] == target) return true;
    if (ids[mid] < target) lo = mid + 1u;
    else hi = mid;
  }
  return false;
}

static void cs_ensure_ow_targets_cache(const ccord_ui *ui, struct ui_channel_settings *cs)
{
  uint64_t rev = ccord_store_revision(ui->d.store);
  if (cs->ow_targets_valid && cs->ow_targets_rev == rev &&
      cs->ow_targets_sel_id == cs->ow_sel_id && cs->ow_targets_sel_kind == cs->ow_sel_kind) {
    return;
  }
  const ccord_store *st = ui->d.store;
  size_t nows = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(st, cs->channel_id, &nows);
  size_t need = 1u + nows + 1u;
  if (need > CCORD_ADMIN_CLONE_OVERWRITES_MAX) need = CCORD_ADMIN_CLONE_OVERWRITES_MAX;
  if (need > cs->ow_target_cap) {
    cs_ow_target_item *nv = ccord_realloc(cs->ow_targets, need * sizeof *nv);
    if (!nv) {
      cs->ow_target_count = 0;
      cs->ow_sel_has_stored = false;
      return;
    }
    cs->ow_targets = nv;
    cs->ow_target_cap = need;
  }

  size_t count = 0;
  bool sel_stored = false;
  cs->ow_targets[count++] = (cs_ow_target_item){
    .id = cs->guild_id, .kind = CCORD_OVERWRITE_ROLE, .label = "@everyone"
  };

  for (size_t i = 0; ows && i < nows; i++) {
    if (ows[i].id == cs->ow_sel_id) sel_stored = true;
    if (ows[i].id == cs->guild_id || count >= CCORD_ADMIN_CLONE_OVERWRITES_MAX) continue;
    cs_ow_target_item it = {.id = ows[i].id, .kind = ows[i].kind};
    if (ows[i].kind == CCORD_OVERWRITE_ROLE) {
      const ccord_role *ro = ccord_store_role(st, cs->guild_id, ows[i].id);
      (void)snprintf(it.label, sizeof it.label, "@%s", (ro && ro->name && ro->name[0]) ? ro->name : "role");
    } else {
      const char *mn = ui_member_display_name(ui, cs->guild_id, ows[i].id);
      (void)snprintf(it.label, sizeof it.label, "%s", mn && mn[0] ? mn : "Member");
    }
    cs->ow_targets[count++] = it;
  }

  if (cs->ow_sel_id != CCORD_ID_NONE && cs->ow_sel_id != cs->guild_id && !sel_stored &&
      count < CCORD_ADMIN_CLONE_OVERWRITES_MAX) {
    cs_ow_target_item it = {.id = cs->ow_sel_id, .kind = cs->ow_sel_kind};
    if (cs->ow_sel_kind == CCORD_OVERWRITE_ROLE) {
      const ccord_role *ro = ccord_store_role(st, cs->guild_id, cs->ow_sel_id);
      (void)snprintf(it.label, sizeof it.label, "@%s", (ro && ro->name && ro->name[0]) ? ro->name : "role");
    } else {
      const char *mn = ui_member_display_name(ui, cs->guild_id, cs->ow_sel_id);
      (void)snprintf(it.label, sizeof it.label, "%s", mn && mn[0] ? mn : "Member");
    }
    cs->ow_targets[count++] = it;
  }

  cs->ow_target_count = count;
  cs->ow_sel_has_stored = sel_stored;
  cs->ow_targets_rev = rev;
  cs->ow_targets_sel_id = cs->ow_sel_id;
  cs->ow_targets_sel_kind = cs->ow_sel_kind;
  cs->ow_targets_valid = true;
}

static void cs_ensure_ow_add_cache(const ccord_ui *ui, struct ui_channel_settings *cs)
{
  uint64_t rev = ccord_store_revision(ui->d.store);
  if (cs->ow_add_valid && cs->ow_add_rev == rev) return;

  cs->ow_add_role_count = 0;
  cs->ow_add_member_count = 0;

  // Build sorted overwrite id table once so role/member membership tests are O(log O) instead of O(R x O).
  size_t nows = 0;
  const ccord_overwrite *ows = ccord_store_channel_overwrites(ui->d.store, cs->channel_id, &nows);
  ccord_id stack_ids[64];
  ccord_id *ow_ids = stack_ids;
  if (nows > 64u) {
    ow_ids = ccord_malloc(nows * sizeof *ow_ids);
    if (!ow_ids) return;
  }
  for (size_t i = 0; i < nows; i++) ow_ids[i] = ows[i].id;
  if (nows > 1u) qsort(ow_ids, nows, sizeof *ow_ids, cs_cmp_id);

  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(ui->d.store, cs->guild_id, &nroles);
  for (size_t i = 0; roles && i < nroles; i++) {
    if (roles[i].id == cs->guild_id) continue;
    if (nows > 0 && cs_id_in_sorted(ow_ids, nows, roles[i].id)) continue;
    if (cs->ow_add_role_count == cs->ow_add_role_cap) {
      size_t ncap = cs->ow_add_role_cap ? cs->ow_add_role_cap * 2u : 16u;
      cs_ow_add_item *nv = ccord_realloc(cs->ow_add_roles, ncap * sizeof *nv);
      if (!nv) break;
      cs->ow_add_roles = nv;
      cs->ow_add_role_cap = ncap;
    }
    cs_ow_add_item *it = &cs->ow_add_roles[cs->ow_add_role_count++];
    it->id = roles[i].id;
    (void)snprintf(it->label, sizeof it->label, "+ @%s", roles[i].name ? roles[i].name : "role");
  }

  size_t nmemb = ccord_store_member_count(ui->d.store, cs->guild_id);
  for (size_t i = 0; i < nmemb && i < 24u; i++) {
    const ccord_member *mb = ccord_store_member_at(ui->d.store, cs->guild_id, i);
    if (!mb || (nows > 0 && cs_id_in_sorted(ow_ids, nows, mb->user_id))) continue;
    if (cs->ow_add_member_count == cs->ow_add_member_cap) {
      size_t ncap = cs->ow_add_member_cap ? cs->ow_add_member_cap * 2u : 16u;
      cs_ow_add_item *nv = ccord_realloc(cs->ow_add_members, ncap * sizeof *nv);
      if (!nv) break;
      cs->ow_add_members = nv;
      cs->ow_add_member_cap = ncap;
    }
    cs_ow_add_item *it = &cs->ow_add_members[cs->ow_add_member_count++];
    it->id = mb->user_id;
    (void)snprintf(it->label, sizeof it->label, "+ %s", ui_member_display_name(ui, cs->guild_id, mb->user_id));
  }

  if (ow_ids != stack_ids) ccord_free(ow_ids);
  cs->ow_add_rev = rev;
  cs->ow_add_valid = true;
}

// A tri-state permission button is a button: danger for deny, success for allow, secondary for inherit / unset.
static void draw_tristate_btn(cs_cx *c, ccord_rect r, const char *glyph, int kind, bool active, const char *why,
                              cs_act act, uint64_t bit, const char *name)
{
  // The active choice: red for deny, green for allow, the selected fill for inherit.
  uis_btn_kind bk = UIS_BTN_SECONDARY;
  if (active && kind < 0) bk = UIS_BTN_DANGER;
  else if (active && kind > 0) bk = UIS_BTN_SUCCESS;
  uis_btn_size size = cs_btn_size(r.h);
  r.h = uis_button_height(size);
  if (!cs_visible(c, r)) return;
  bool hov = cs_add_hit(c, r, act, bit, why, name);
  uis_button(c->ui, c->r, r, glyph, bk, size,
             (uis_state){.hover = hov, .selected = active && kind == 0, .disabled = why != nullptr}, "csbtn",
             ui_name_id(name));
}

void cs_draw_cs_permissions(cs_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_channel_settings *cs = c->cs;
  float y = *py;

  (void)cs_text(c, "Channel Permissions", x, y, CS_TITLE(c), true, COL(text_header), w);
  y += 28.0f;
  (void)cs_text(c, "Use permissions to customise who can do what in this channel.", x, y, CS_CAP(c), false,
                COL(text_muted), w);
  y += 28.0f;

  cs_label(c, "ROLES / MEMBERS", x, y);
  cs_button(c, (ccord_rect){x + w - 154.0f, y - 4.0f, 154.0f, 26.0f},
            cs->ow_add_open ? "Close Add Menu" : "+ Add Role / Member",
            UIS_BTN_PRIMARY, nullptr, CA_OW_ADD_TOGGLE, 0, "ow_add_toggle");
  y += 28.0f;

  if (cs->ow_add_open) {
    cs_ensure_ow_add_cache(ui, cs);
    float cx = x;
    for (size_t i = 0; i < cs->ow_add_role_count; i++) {
      const cs_ow_add_item *it = &cs->ow_add_roles[i];
      char nm[CS_NAME_LEN];
      float cw = 128.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 32.0f;
      }
      cs_chip(c, (ccord_rect){cx, y, cw, 26.0f}, it->label, false, nullptr, CA_OW_ADD_ROLE, it->id,
              NAME(nm, "ow_add_role:%llu", (unsigned long long)it->id));
      cx += cw + 6.0f;
    }
    if (cx > x) {
      cx = x;
      y += 32.0f;
    }
    for (size_t i = 0; i < cs->ow_add_member_count; i++) {
      const cs_ow_add_item *it = &cs->ow_add_members[i];
      char nm[CS_NAME_LEN];
      float cw = 128.0f;
      if (cx + cw > x + w) {
        cx = x;
        y += 32.0f;
      }
      cs_chip(c, (ccord_rect){cx, y, cw, 26.0f}, it->label, false, nullptr, CA_OW_ADD_MEMBER, it->id,
              NAME(nm, "ow_add_member:%llu", (unsigned long long)it->id));
      cx += cw + 6.0f;
    }
    if (cx > x) y += 32.0f;
    y += 8.0f;
  }

  cs_ensure_ow_targets_cache(ui, cs);
  cs->dbg_rows = cs->ow_target_count;
  float cx = x;
  for (size_t i = 0; i < cs->ow_target_count; i++) {
    const cs_ow_target_item *t = &cs->ow_targets[i];
    float cw = 132.0f;
    if (cx + cw > x + w) {
      cx = x;
      y += 34.0f;
    }
    char nm[CS_NAME_LEN];
    cs_chip(c, (ccord_rect){cx, y, cw, 28.0f}, t->label, t->id == cs->ow_sel_id, nullptr, CA_OW_TARGET,
            t->id | ((uint64_t)t->kind << 63u),
            NAME(nm, "ow_target:%llu", (unsigned long long)t->id));
    cx += cw + 6.0f;
  }
  y += 40.0f;

  if (cs->ow_sel_has_stored) {
    cs_button(c, (ccord_rect){x + w - 154.0f, y - 4.0f, 154.0f, 28.0f}, "Remove Overwrite", UIS_BTN_DANGER, nullptr, CA_OW_DELETE, cs->ow_sel_id, "ow_delete");
    y += 32.0f;
  }

  uint64_t ch_perms = cs_cached_chan_perms(ui, cs);
  bool exact = cs->cached_exact;

  size_t nperms = 0;
  const ccord_perm_info *tbl = ccord_perm_table(&nperms);
  ccord_perm_group last_group = CCORD_PERM_GROUP_COUNT;
  for (size_t i = 0; i < nperms; i++) {
    if (tbl[i].group != last_group) {
      last_group = tbl[i].group;
      y += 8.0f;
      char head[64];
      const char *gl = ccord_perm_group_label(last_group);
      size_t k = 0;
      for (; gl[k] && k + 1 < sizeof head; k++) head[k] = (char)((gl[k] >= 'a' && gl[k] <= 'z') ? gl[k] - 32 : gl[k]);
      head[k] = '\0';
      cs_label(c, head, x, y);
      y += 24.0f;
    }
    ccord_rect row = {x, y, w, CS_PERM_ROW_H};
    y += CS_PERM_ROW_H;
    if (!cs_visible(c, row)) continue;

    uint64_t bit = tbl[i].bit;
    bool is_allow = (cs->ow_work.allow & bit) != 0;
    bool is_deny = (cs->ow_work.deny & bit) != 0;
    bool is_inherit = !is_allow && !is_deny;
    const char *why = (exact && !ccord_perm_has(ch_perms, bit))
                        ? "You can't grant or deny permissions you don't hold in this channel."
                        : nullptr;

    (void)cs_text(c, tbl[i].label, row.x, row.y + 6.0f, uis_tokens_of(ui).fs_body, true, COL(text_header), w - 120.0f);
    (void)cs_text(c, tbl[i].description, row.x, row.y + 6.0f + ui_line_h(ui, uis_tokens_of(ui).fs_body), CS_CAP(c),
                  false, COL(text_muted), w - 120.0f);

    float bx = row.x + w - 102.0f;
    float by = row.y + 12.0f;
    char nm[CS_NAME_LEN];
    draw_tristate_btn(c, (ccord_rect){bx, by, 30.0f, 28.0f}, "X", -1, is_deny, why, CA_OW_DENY, bit,
                      NAME(nm, "ow_deny:%llx", (unsigned long long)bit));
    draw_tristate_btn(c, (ccord_rect){bx + 34.0f, by, 30.0f, 28.0f}, "/", 0, is_inherit, why, CA_OW_INHERIT, bit,
                      NAME(nm, "ow_inherit:%llx", (unsigned long long)bit));
    draw_tristate_btn(c, (ccord_rect){bx + 68.0f, by, 30.0f, 28.0f}, "V", 1, is_allow, why, CA_OW_ALLOW, bit,
                      NAME(nm, "ow_allow:%llx", (unsigned long long)bit));
    uis_divider(ui, c->r, row.x, row.y + row.h - 1.0f, row.w);
  }

  *py = y;
}

// ---------------------------------------------------------------- Invites page

void cs_draw_cs_invites(cs_cx *c, float x, float w, float *py)
{
  ccord_ui *ui = c->ui;
  struct ui_channel_settings *cs = c->cs;
  float y = *py;

  (void)cs_text(c, "Channel Invites", x, y, CS_TITLE(c), true, COL(text_header), w - 100.0f);
  cs_button(c, (ccord_rect){x + w - 88.0f, y, 88.0f, 30.0f}, "Refresh", UIS_BTN_SECONDARY,
            nullptr, CA_REFRESH, 0, "refresh");
  y += 40.0f;

  bool can_create = ccord_perm_has(cs_cached_chan_perms(ui, cs), CCORD_PERM_CREATE_INSTANT_INVITE);
  if (can_create) {
    char age_lbl[64], use_lbl[64];
    (void)snprintf(age_lbl, sizeof age_lbl, "Expire: %s", cs_k_cs_inv_age_labels[cs->inv_age_idx]);
    (void)snprintf(use_lbl, sizeof use_lbl, "Max: %s", cs_k_cs_inv_use_labels[cs->inv_uses_idx]);
    if (w < 460.0f) {
      float half = floorf((w - 8.0f) * 0.5f);
      cs_chip(c, (ccord_rect){x, y, half, 30.0f}, age_lbl, false, nullptr, CA_INV_AGE, 0, "inv_age");
      cs_chip(c, (ccord_rect){x + half + 8.0f, y, w - half - 8.0f, 30.0f}, use_lbl, false, nullptr, CA_INV_USES, 0, "inv_uses");
      y += 38.0f;
      cs_button(c, (ccord_rect){x, y, fminf(148.0f, w), 30.0f}, "Create Invite", UIS_BTN_PRIMARY,
                nullptr, CA_INV_CREATE, 0, "inv_create");
      y += 42.0f;
    } else {
      cs_chip(c, (ccord_rect){x, y, 160.0f, 30.0f}, age_lbl, false, nullptr, CA_INV_AGE, 0, "inv_age");
      cs_chip(c, (ccord_rect){x + 168.0f, y, 150.0f, 30.0f}, use_lbl, false, nullptr, CA_INV_USES, 0, "inv_uses");
      cs_button(c, (ccord_rect){x + 326.0f, y, 128.0f, 30.0f}, "Create Invite", UIS_BTN_PRIMARY,
                nullptr, CA_INV_CREATE, 0, "inv_create");
      y += 46.0f;
    }
  }

  size_t count = 0;
  const ccord_invite *invs = ccord_store_channel_invites(ui->d.store, cs->guild_id, cs->channel_id, &count);
  cs->dbg_rows = count;
  if (!ccord_store_channel_invites_loaded(ui->d.store, cs->guild_id, cs->channel_id)) {
    const char *msg = cs->invites_fail_ms ? "Couldn't load channel invites. Try Refresh." : "Loading channel invites...";
    (void)cs_text(c, msg, x, y, uis_tokens_of(ui).fs_body, false, COL(text_muted), w);
    *py = y + 32.0f;
    return;
  }
  if (count == 0) {
    (void)cs_text(c, "No active invites for this channel.", x, y, uis_tokens_of(ui).fs_body, false, COL(text_muted), w);
    *py = y + 32.0f;
    return;
  }

  for (size_t i = 0; i < count; i++) {
    const ccord_invite *inv = &invs[i];
    ccord_rect row = {x, y, w, 46.0f};
    y += 50.0f;
    if (!cs_visible(c, row)) continue;
    ccord_draw_rrect(c->r, row, uis_tokens_of(ui).r_control, COL(bg_secondary));
    char code_line[128], meta_line[128];
    (void)snprintf(code_line, sizeof code_line, "discord.gg/%s", inv->code);
    // Streamer Mode: mask the invite code while Hide Instant Invite Links is on.
    char code_masked[160];
    size_t code_ml = ui_streamer_mask_invites(ui, code_line, strlen(code_line), code_masked, sizeof code_masked, true);
    const char *shown_code = code_ml > 0 ? code_masked : code_line;
    const ccord_user *u = inv->inviter_id ? ccord_store_user(ui->d.store, inv->inviter_id) : nullptr;
    if (inv->max_uses > 0) {
      (void)snprintf(meta_line, sizeof meta_line, "by %s  •  %u / %u uses",
                     (u && u->name) ? u->name : "Unknown", inv->uses, inv->max_uses);
    } else {
      (void)snprintf(meta_line, sizeof meta_line, "by %s  •  %u uses",
                     (u && u->name) ? u->name : "Unknown", inv->uses);
    }
    (void)cs_text(c, shown_code, row.x + 12.0f, row.y + 5.0f, uis_tokens_of(ui).fs_body, true, COL(text_header), w - 180.0f);
    (void)cs_text(c, meta_line, row.x + 12.0f, row.y + 24.0f, CS_CAP(c), false, COL(text_muted), w - 180.0f);
    char nm[CS_NAME_LEN];
    cs_button(c, (ccord_rect){row.x + w - 156.0f, row.y + 9.0f, 68.0f, 28.0f}, "Copy", UIS_BTN_SECONDARY, nullptr, CA_INV_COPY, i, NAME(nm, "inv_copy:%s", inv->code));
    cs_button(c, (ccord_rect){row.x + w - 80.0f, row.y + 9.0f, 70.0f, 28.0f}, "Revoke", UIS_BTN_DANGER, nullptr, CA_INV_REVOKE, i, NAME(nm, "inv_revoke:%s", inv->code));
  }

  *py = y;
}

