// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "ui_core_internal.h"

// ---------------------------------------------------------------- caches (channel rows, member rows)

bool ui_is_collapsed(const ccord_ui *ui, ccord_id category_id)
{
  for (size_t i = 0; i < ui->ncollapsed; i++) {
    if (ui->collapsed[i] == category_id) return true;
  }
  return false;
}

void uc_toggle_collapsed(ccord_ui *ui, ccord_id id)
{
  for (size_t i = 0; i < ui->ncollapsed; i++) {
    if (ui->collapsed[i] == id) {
      ui->collapsed[i] = ui->collapsed[--ui->ncollapsed];
      return;
    }
  }
  if (ui->ncollapsed == ui->capcollapsed) {
    size_t ncap = ui->capcollapsed ? ui->capcollapsed * 2 : 8;
    ccord_id *n = ccord_realloc_array(ui->collapsed, ncap, sizeof *n);
    if (!n) {
      ui_notice(ui, "Out of memory");
      return;
    }
    ui->collapsed = n;
    ui->capcollapsed = ncap;
  }
  ui->collapsed[ui->ncollapsed++] = id;
}

static bool grow_crow(ccord_ui *ui, size_t need)
{
  if (need <= ui->capcrow) return true;
  size_t ncap = ui->capcrow ? ui->capcrow : 64;
  while (ncap < need) ncap *= 2;
  chan_row *n = ccord_realloc_array(ui->crow, ncap, sizeof *n);
  if (!n) return false;
  ui->crow = n;
  ui->capcrow = ncap;
  return true;
}

static bool has_selected_thread(const ccord_store *s, ccord_id g, ccord_id text_id, ccord_id sel)
{
  if (sel == CCORD_ID_NONE) return false;
  const ccord_channel *sc = ccord_store_channel(s, sel);
  return sc && sc->guild_id == g && sc->kind == CCORD_CHANNEL_TEXT && sc->parent_id == text_id;
}

typedef struct chan_idx_map {
  const ccord_channel **chs;
  uint32_t *parent_idx;
  uint32_t *thread_head;
  uint32_t *thread_tail;
  uint32_t *thread_next;
  uint32_t *cat_head;
  uint32_t *cat_tail;
  uint32_t *cat_next;
  ccord_id *ht_ids;
  uint32_t *ht_idx;
  size_t ht_mask;
  bool heap_alloc;
} chan_idx_map;

static uint32_t chan_ht_find(const chan_idx_map *m, ccord_id id)
{
  if (id == CCORD_ID_NONE || !m->ht_ids) return UINT32_MAX;
  size_t slot = (size_t)(id * 11400714819323198485ull) & m->ht_mask;
  for (;;) {
    ccord_id k = m->ht_ids[slot];
    if (k == id) return m->ht_idx[slot];
    if (k == CCORD_ID_NONE) return UINT32_MAX;
    slot = (slot + 1u) & m->ht_mask;
  }
}

static bool cm_is_thread(const chan_idx_map *cm, size_t i)
{
  const ccord_channel *ch = cm->chs[i];
  uint32_t p = cm->parent_idx[i];
  return ch && ch->kind == CCORD_CHANNEL_TEXT && p != UINT32_MAX &&
         cm->chs[p] && cm->chs[p]->kind == CCORD_CHANNEL_TEXT;
}

static bool cm_is_cat_child(const chan_idx_map *cm, size_t i)
{
  uint32_t p = cm->parent_idx[i];
  return p != UINT32_MAX && cm->chs[p] && cm->chs[p]->kind == CCORD_CHANNEL_CATEGORY;
}

static bool append_channel_and_children(ccord_ui *ui, ccord_id g, const chan_idx_map *cm, size_t i,
                                        const ccord_channel *ch, float *py)
{
  bool is_cat = ch->kind == CCORD_CHANNEL_CATEGORY;
  if (!grow_crow(ui, ui->ncrow + 1)) return false;
  float chan_h = M(channel_item_height);
  if (ui->mobile_layout && chan_h < 42.0f) chan_h = 42.0f;
  float h = is_cat ? M(category_height) : (g == CCORD_DM_GUILD_ID ? (ui->mobile_layout ? 52.0f : 48.0f) : chan_h);
  ui->crow[ui->ncrow++] = (chan_row){is_cat, false, false, false, i, 0, *py, h, DM_NAV_NONE};
  *py += h + (is_cat ? 0.0f : (g == CCORD_DM_GUILD_ID ? 3.0f : 2.0f));
  if (!is_cat && ch->kind == CCORD_CHANNEL_TEXT && cm->thread_head) {
    bool is_forum = ccord_store_channel_is_forum(ui->d.store, ch->id);
    ccord_id sel_cid = ccord_store_selected_channel(ui->d.store);
    for (uint32_t t = cm->thread_head[i]; t != UINT32_MAX; t = cm->thread_next[t]) {
      const ccord_channel *tc = cm->chs[t];
      if (!tc) continue;
      if ((is_forum || tc->thread_archived) && tc->id != sel_cid) continue;
      if (!grow_crow(ui, ui->ncrow + 1)) return false;
      float th = ui->mobile_layout ? (M(channel_item_height) < 40.0f ? 40.0f : M(channel_item_height)) : (M(channel_item_height) - 4.0f);
      ui->crow[ui->ncrow++] = (chan_row){false, false, false, true, (size_t)t, 0, *py, th, DM_NAV_NONE};
      *py += th + 2.0f;
    }
  }
  if (!is_cat && (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE) &&
      !ui_voice_names_hidden(ui, ch->id)) {
    size_t nv = ccord_store_voice_count(ui->d.store, ch->id);
    for (size_t v = 0; v < nv; v++) {
      const ccord_voice_state *vs = ccord_store_voice_at(ui->d.store, ch->id, v);
      if (!vs) continue;
      if (!grow_crow(ui, ui->ncrow + 1)) return false;
      float vh = ui->mobile_layout ? 38.0f : 26.0f;
      ui->crow[ui->ncrow++] = (chan_row){false, true, false, false, i, vs->user_id, *py, vh, DM_NAV_NONE};
      *py += vh;
    }
  }
  return true;
}

typedef struct dm_sort_item {
  size_t idx;
  ccord_id last_message_id;
  ccord_id id;
  bool pinned;
} dm_sort_item;

static int cmp_dm_sort(const void *pa, const void *pb)
{
  const dm_sort_item *a = pa, *b = pb;
  if (a->pinned != b->pinned) return a->pinned ? -1 : 1;
  ccord_id ka = a->last_message_id ? a->last_message_id : a->id;
  ccord_id kb = b->last_message_id ? b->last_message_id : b->id;
  if (ka != kb) {
    return ka > kb ? -1 : 1;
  }
  if (a->id != b->id) {
    return a->id > b->id ? -1 : 1;
  }
  return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

bool ui_role_preview_active_for(const ccord_ui *ui, ccord_id guild_id)
{
  return ui && ui->d.store && guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID &&
         ui->role_preview_guild == guild_id && ccord_store_roles_known(ui->d.store, guild_id);
}

uint64_t ui_effective_permissions(const ccord_ui *ui, ccord_id guild_id, ccord_id channel_id)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return 0;
  const ccord_store *st = ui->d.store;
  if (!ui_role_preview_active_for(ui, guild_id)) {
    return ccord_store_permissions(st, guild_id, channel_id, ccord_store_self_id(st), ui->now_unix_ms);
  }
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(st, guild_id, &nroles);
  if (!roles) return 0;
  ccord_id rid = ui->role_preview_role_id;
  const ccord_id *mroles = nullptr;
  size_t nmroles = 0;
  if (rid != CCORD_ID_NONE && rid != guild_id && ccord_store_role(st, guild_id, rid) != nullptr) {
    mroles = &rid;
    nmroles = 1u;
  }
  uint64_t perms = ccord_perm_guild(guild_id, CCORD_ID_NONE, CCORD_ID_NONE, roles, nroles, mroles, nmroles);
  if (channel_id != CCORD_ID_NONE) {
    const ccord_channel *c = ccord_store_channel(st, channel_id);
    if (c && c->guild_id == guild_id) {
      ccord_id source = (ccord_discord_type_is_thread(c->discord_type) && c->parent_id != CCORD_ID_NONE)
                          ? c->parent_id
                          : channel_id;
      size_t now = 0;
      const ccord_overwrite *ow = ccord_store_channel_overwrites(st, source, &now);
      perms = ccord_perm_channel(perms, guild_id, CCORD_ID_NONE, ow, now, mroles, nmroles);
    }
  }
  return perms;
}

// Discord does not list what you may not see. Only on exact data (a guess never hides anything), never the channel you
// are looking at (except in Role Preview, which shows what that role sees), and a category stays while you may view it
// or anything inside it. `vis` has one entry per channel of the guild, in store order; nullptr means "show everything".
static bool *compute_channel_visibility(const ccord_ui *ui, ccord_id g, size_t n, const chan_idx_map *cm)
{
  if (n == 0 || g == CCORD_DM_GUILD_ID || g == CCORD_ID_NONE) return nullptr;
  const ccord_store *st = ui->d.store;
  bool preview = ui_role_preview_active_for(ui, g);
  ccord_id self = ccord_store_self_id(st);
  if (!preview && !ccord_store_permissions_exact(st, g, self)) return nullptr;
  bool *vis = ccord_alloc_array(n, sizeof *vis);
  if (!vis) return nullptr;
  ccord_id sel = ccord_store_selected_channel(st);
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = cm->chs[i];
    vis[i] = !ch || (!preview && ch->id == sel) ||
             (ui_effective_permissions(ui, g, ch->id) & CCORD_PERM_VIEW_CHANNEL) != 0;
  }
  // O(C) pass: any visible non-category child keeps its parent category visible.
  for (size_t k = 0; k < n; k++) {
    const ccord_channel *ch = cm->chs[k];
    if (!ch || ch->kind == CCORD_CHANNEL_CATEGORY || !vis[k]) continue;
    uint32_t p = cm->parent_idx[k];
    if (p != UINT32_MAX && cm->chs[p] && cm->chs[p]->kind == CCORD_CHANNEL_CATEGORY) {
      vis[p] = true;
    }
  }
  return vis;
}

bool ui_channel_visible(const ccord_ui *ui, ccord_id guild, const ccord_channel *ch)
{
  if (!ch) return false;
  if (guild == CCORD_ID_NONE || guild == CCORD_DM_GUILD_ID) return true;
  const ccord_store *st = ui->d.store;
  bool preview = ui_role_preview_active_for(ui, guild);
  ccord_id self = ccord_store_self_id(st);
  if (!preview && (!ccord_store_permissions_exact(st, guild, self) || ch->id == ccord_store_selected_channel(st))) {
    return true;
  }
  return (ui_effective_permissions(ui, guild, ch->id) & CCORD_PERM_VIEW_CHANNEL) != 0;
}

const char *ui_voice_lock_reason_ex(const ccord_ui *ui, ccord_id guild_id, const ccord_channel *ch,
                                    ui_voice_lock_kind *out_kind)
{
  if (out_kind) *out_kind = UI_VOICE_LOCK_NONE;
  if (!ui || !ui->d.store || !ch) return nullptr;
  if (ch->kind != CCORD_CHANNEL_VOICE && ch->kind != CCORD_CHANNEL_STAGE) return nullptr;
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return nullptr;
  const ccord_store *st = ui->d.store;
  if (ui_role_preview_active_for(ui, guild_id)) {
    uint64_t perms = ui_effective_permissions(ui, guild_id, ch->id);
    if (!ccord_perm_has(perms, CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CONNECT)) {
      if (out_kind) *out_kind = UI_VOICE_LOCK_PERMISSION;
      return "Selected role does not have permission to connect.";
    }
    if (ch->kind == CCORD_CHANNEL_VOICE && ch->user_limit > 0 && !ccord_perm_has(perms, CCORD_PERM_MOVE_MEMBERS) &&
        ccord_store_voice_count(st, ch->id) >= ch->user_limit) {
      if (out_kind) *out_kind = UI_VOICE_LOCK_FULL;
      return "Voice channel is full.";
    }
    return nullptr;
  }
  const ccord_voice_state *self_vs = ui_self_voice(ui);
  if (self_vs && self_vs->channel_id == ch->id) return nullptr;
  if (ui_self_pending(ui, guild_id)) {
    if (out_kind) *out_kind = UI_VOICE_LOCK_PERMISSION;
    return "Complete the server rules to join voice.";
  }
  ccord_id self = ccord_store_self_id(st);
  if (!ccord_store_permissions_exact(st, guild_id, self)) return nullptr;
  uint64_t perms = ccord_store_permissions(st, guild_id, ch->id, self, ui->now_unix_ms);
  if (!ccord_perm_has(perms, CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CONNECT)) {
    if (out_kind) *out_kind = UI_VOICE_LOCK_PERMISSION;
    const ccord_member *me = ccord_store_find_member(st, guild_id, self);
    if (me && me->timeout_until_ms > ui->now_unix_ms) {
      return "You have been timed out and cannot join voice.";
    }
    return "You do not have permission to connect to this voice channel.";
  }
  if (ch->kind == CCORD_CHANNEL_VOICE && ch->user_limit > 0 && !ccord_perm_has(perms, CCORD_PERM_MOVE_MEMBERS) &&
      ccord_store_voice_count(st, ch->id) >= ch->user_limit) {
    if (out_kind) *out_kind = UI_VOICE_LOCK_FULL;
    return "Voice channel is full.";
  }
  return nullptr;
}

const char *ui_voice_lock_reason(const ccord_ui *ui, ccord_id guild_id, const ccord_channel *ch)
{
  return ui_voice_lock_reason_ex(ui, guild_id, ch, nullptr);
}

bool ui_voice_can_join(const ccord_ui *ui, ccord_id guild_id, const ccord_channel *ch)
{
  return ui_voice_lock_reason_ex(ui, guild_id, ch, nullptr) == nullptr;
}

static void chan_idx_free(chan_idx_map *cm)
{
  if (!cm->heap_alloc) return;
  ccord_free(cm->chs);
  ccord_free(cm->parent_idx);
  ccord_free(cm->thread_head);
  ccord_free(cm->thread_tail);
  ccord_free(cm->thread_next);
  ccord_free(cm->cat_head);
  ccord_free(cm->cat_tail);
  ccord_free(cm->cat_next);
  ccord_free(cm->ht_ids);
  ccord_free(cm->ht_idx);
  *cm = (chan_idx_map){0};
}

static void rebuild_channel_rows(ccord_ui *ui)
{
  ui->ncrow = 0;
  ui->dbg_relationship_rows = 0;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  size_t n = ccord_store_channel_count(ui->d.store, g);
  ccord_id sel = ccord_store_selected_channel(ui->d.store);
  float y = 8.0f;
  const ccord_channel_kind kinds[3] = {CCORD_CHANNEL_TEXT, CCORD_CHANNEL_VOICE, CCORD_CHANNEL_STAGE};
  bool *vis = nullptr;

  // Precompute O(C) channel id -> index, parent_idx, thread lists, and category child lists.
  const ccord_channel *s_chs[64];
  uint32_t s_parent[64], s_thead[64], s_ttail[64], s_tnext[64], s_chead[192], s_ctail[192], s_cnext[64];
  ccord_id s_ht_ids[128];
  uint32_t s_ht_idx[128];
  chan_idx_map cm = {0};

  if (n > 0) {
    size_t hcap = 16u;
    while (hcap < n * 2u) hcap *= 2u;
    if (n <= 64u) {
      cm.chs = s_chs;
      cm.parent_idx = s_parent;
      cm.thread_head = s_thead;
      cm.thread_tail = s_ttail;
      cm.thread_next = s_tnext;
      cm.cat_head = s_chead;
      cm.cat_tail = s_ctail;
      cm.cat_next = s_cnext;
      cm.ht_ids = s_ht_ids;
      cm.ht_idx = s_ht_idx;
      cm.ht_mask = hcap - 1u;
      memset(s_ht_ids, 0, hcap * sizeof s_ht_ids[0]);
    } else {
      cm.heap_alloc = true;
      cm.chs = ccord_alloc_array(n, sizeof *cm.chs);
      cm.parent_idx = ccord_alloc_array(n, sizeof *cm.parent_idx);
      cm.thread_head = ccord_alloc_array(n, sizeof *cm.thread_head);
      cm.thread_tail = ccord_alloc_array(n, sizeof *cm.thread_tail);
      cm.thread_next = ccord_alloc_array(n, sizeof *cm.thread_next);
      cm.cat_head = ccord_alloc_array(n * 3u, sizeof *cm.cat_head);
      cm.cat_tail = ccord_alloc_array(n * 3u, sizeof *cm.cat_tail);
      cm.cat_next = ccord_alloc_array(n, sizeof *cm.cat_next);
      cm.ht_ids = ccord_calloc(hcap, sizeof *cm.ht_ids);
      cm.ht_idx = ccord_alloc_array(hcap, sizeof *cm.ht_idx);
      cm.ht_mask = hcap - 1u;
      if (!cm.chs || !cm.parent_idx || !cm.thread_head || !cm.thread_tail || !cm.thread_next ||
          !cm.cat_head || !cm.cat_tail || !cm.cat_next || !cm.ht_ids || !cm.ht_idx) {
        goto done_channels;
      }
    }
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *ch = ccord_store_channel_at(ui->d.store, g, i);
      cm.chs[i] = ch;
      cm.thread_head[i] = UINT32_MAX;
      cm.thread_tail[i] = UINT32_MAX;
      cm.thread_next[i] = UINT32_MAX;
      cm.cat_next[i] = UINT32_MAX;
      cm.cat_head[i * 3u + 0u] = UINT32_MAX;
      cm.cat_head[i * 3u + 1u] = UINT32_MAX;
      cm.cat_head[i * 3u + 2u] = UINT32_MAX;
      cm.cat_tail[i * 3u + 0u] = UINT32_MAX;
      cm.cat_tail[i * 3u + 1u] = UINT32_MAX;
      cm.cat_tail[i * 3u + 2u] = UINT32_MAX;
      if (ch && ch->id != CCORD_ID_NONE) {
        size_t slot = (size_t)(ch->id * 11400714819323198485ull) & cm.ht_mask;
        while (cm.ht_ids[slot] != CCORD_ID_NONE && cm.ht_ids[slot] != ch->id) {
          slot = (slot + 1u) & cm.ht_mask;
        }
        cm.ht_ids[slot] = ch->id;
        cm.ht_idx[slot] = (uint32_t)i;
      }
    }
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *ch = cm.chs[i];
      cm.parent_idx[i] = (ch && ch->parent_id != CCORD_ID_NONE) ? chan_ht_find(&cm, ch->parent_id) : UINT32_MAX;
    }
    for (size_t t = 0; t < n; t++) {
      const ccord_channel *tc = cm.chs[t];
      uint32_t p = cm.parent_idx[t];
      if (!tc || tc->kind != CCORD_CHANNEL_TEXT || p == UINT32_MAX || p == (uint32_t)t) continue;
      if (!cm.chs[p] || cm.chs[p]->kind != CCORD_CHANNEL_TEXT) continue;
      if (cm.thread_head[p] == UINT32_MAX) cm.thread_head[p] = (uint32_t)t;
      else cm.thread_next[cm.thread_tail[p]] = (uint32_t)t;
      cm.thread_tail[p] = (uint32_t)t;
    }
  }

  if (g == CCORD_DM_GUILD_ID) {
    bool show_requests = ui->dm_requests_view;
    for (size_t i = 0; i < n && !show_requests; i++) {
      const ccord_channel *ch = cm.chs[i];
      if (ch && ch->is_message_request) show_requests = true;
    }
    if (grow_crow(ui, ui->ncrow + (show_requests ? 5u : 4u))) {
      float ih = ui->mobile_layout ? 42.0f : 36.0f;
      ui->crow[ui->ncrow++] = (chan_row){false, false, false, false, 0, 0, y, ih, DM_NAV_FRIENDS};
      y += ih + 2.0f;
      if (show_requests) {
        ui->crow[ui->ncrow++] = (chan_row){false, false, false, false, 0, 0, y, ih, DM_NAV_MESSAGE_REQUESTS};
        y += ih + 2.0f;
      }
      ui->crow[ui->ncrow++] = (chan_row){false, false, false, false, 0, 0, y, ih, DM_NAV_NITRO};
      y += ih + 2.0f;
      ui->crow[ui->ncrow++] = (chan_row){false, false, false, false, 0, 0, y, ih, DM_NAV_SHOP};
      y += ih + 6.0f;
      float ch_h = M(category_height);
      ui->crow[ui->ncrow++] = (chan_row){true, false, false, false, 0, 0, y, ch_h, DM_NAV_DM_HEADER};
      y += ch_h;
    }
    if (n > 0) {
      dm_sort_item *dms = ccord_alloc_array(n, sizeof *dms);
      if (dms) {
        size_t ndm = 0;
        for (size_t i = 0; i < n; i++) {
          const ccord_channel *ch = cm.chs[i];
          if (!ch || ch->is_message_request || ch->kind != CCORD_CHANNEL_TEXT || cm_is_thread(&cm, i)) continue;
          bool pin = ui_dm_is_pinned(ui, ch->id) || ui_dm_is_favorite(ui, ch->id);
          dms[ndm++] = (dm_sort_item){i, ch->last_message_id, ch->id, pin};
        }
        qsort(dms, ndm, sizeof *dms, cmp_dm_sort);
        for (size_t k = 0; k < ndm; k++) {
          const ccord_channel *ch = cm.chs[dms[k].idx];
          if (ch && !append_channel_and_children(ui, g, &cm, dms[k].idx, ch, &y)) break;
        }
        ccord_free(dms);
      }
    }
    goto done_channels;
  }

  vis = compute_channel_visibility(ui, g, n, &cm);
  bool hide_muted = ui_guild_hide_muted(ui, g);
  const ccord_voice_state *self_vs = ui_self_voice(ui);
  ccord_id self_vc = self_vs ? self_vs->channel_id : CCORD_ID_NONE;
#define CHANNEL_SHOWN(i) (!vis || vis[i])
#define CHANNEL_MUTED_SKIP(ch) \
  ((ch) && (ch)->kind != CCORD_CHANNEL_CATEGORY && \
   (hide_muted && ui_channel_muted(ui, (ch))) && \
   (ch)->id != sel && (ch)->mention_count == 0 && (ch)->id != self_vc && \
   !ui_channel_is_pinned(ui, (ch)->id) && !has_selected_thread(ui->d.store, g, (ch)->id, sel))

  // Pinned guild channels appear first at the top of the server's channel list.
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = cm.chs[i];
    if (!ch || ch->kind == CCORD_CHANNEL_CATEGORY || cm_is_thread(&cm, i) || !CHANNEL_SHOWN(i)) continue;
    if (!ui_channel_is_pinned(ui, ch->id)) continue;
    if (!append_channel_and_children(ui, g, &cm, i, ch, &y)) goto done_channels;
  }

  // Group categorized channels into per-(category, kind) order-preserving lists in one O(C) pass.
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = cm.chs[i];
    if (!ch || cm_is_thread(&cm, i) || !cm_is_cat_child(&cm, i) || !CHANNEL_SHOWN(i)) continue;
    if (ui_channel_is_pinned(ui, ch->id) || CHANNEL_MUTED_SKIP(ch)) continue;
    size_t kb = 3u;
    if (ch->kind == CCORD_CHANNEL_TEXT) kb = 0u;
    else if (ch->kind == CCORD_CHANNEL_VOICE) kb = 1u;
    else if (ch->kind == CCORD_CHANNEL_STAGE) kb = 2u;
    if (kb < 3u) {
      size_t bucket = (size_t)cm.parent_idx[i] * 3u + kb;
      if (cm.cat_head[bucket] == UINT32_MAX) cm.cat_head[bucket] = (uint32_t)i;
      else cm.cat_next[cm.cat_tail[bucket]] = (uint32_t)i;
      cm.cat_tail[bucket] = (uint32_t)i;
    }
  }

  // Uncategorized channels (parent_id == NONE or not matching any category in this guild) are never hidden by collapsed categories.
  for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *ch = cm.chs[i];
      if (!ch || ch->kind != kinds[k] || cm_is_thread(&cm, i) || !CHANNEL_SHOWN(i)) continue;
      if (cm_is_cat_child(&cm, i) || ui_channel_is_pinned(ui, ch->id) || CHANNEL_MUTED_SKIP(ch)) continue;
      if (!append_channel_and_children(ui, g, &cm, i, ch, &y)) goto done_channels;
    }
  }

  // Categories in store order, each immediately followed by its child text, voice and stage channels.
  for (size_t ci = 0; ci < n; ci++) {
    const ccord_channel *cat = cm.chs[ci];
    if (!cat || cat->kind != CCORD_CHANNEL_CATEGORY || !CHANNEL_SHOWN(ci)) continue;
    bool has_any_child = cm.cat_head[ci * 3u + 0u] != UINT32_MAX || cm.cat_head[ci * 3u + 1u] != UINT32_MAX ||
                         cm.cat_head[ci * 3u + 2u] != UINT32_MAX;
    if (hide_muted && ui_channel_muted(ui, cat) && !has_any_child) continue;
    if (!append_channel_and_children(ui, g, &cm, ci, cat, &y)) goto done_channels;
    bool collapsed = ui_is_collapsed(ui, cat->id);
    for (size_t kb = 0; kb < 3u; kb++) {
      for (uint32_t idx = cm.cat_head[ci * 3u + kb]; idx != UINT32_MAX; idx = cm.cat_next[idx]) {
        const ccord_channel *ch = cm.chs[idx];
        if (!ch) continue;
        if (collapsed && ch->id != sel && !has_selected_thread(ui->d.store, g, ch->id, sel)) continue;
        if (!append_channel_and_children(ui, g, &cm, (size_t)idx, ch, &y)) goto done_channels;
      }
    }
  }

done_channels:
#undef CHANNEL_MUTED_SKIP
#undef CHANNEL_SHOWN
  chan_idx_free(&cm);
  ccord_free(vis);
  if (g == CCORD_DM_GUILD_ID) {
    size_t nrel = ccord_store_relationship_count(ui->d.store);
    size_t active_rel = 0;
    for (size_t i = 0; i < nrel; i++) {
      const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
      if (rel && rel->kind != CCORD_RELATIONSHIP_NONE) active_rel++;
    }
    if (active_rel > 0 && grow_crow(ui, ui->ncrow + 1 + active_rel)) {
      float ch_h = M(category_height);
      ui->crow[ui->ncrow++] = (chan_row){true, false, true, false, 0, 0, y, ch_h, DM_NAV_NONE};
      y += ch_h;
      for (size_t i = 0; i < nrel; i++) {
        const ccord_relationship *rel = ccord_store_relationship_at(ui->d.store, i);
        if (!rel || rel->kind == CCORD_RELATIONSHIP_NONE) continue;
        float rh = 42.0f;
        ui->crow[ui->ncrow++] = (chan_row){false, false, true, false, i, rel->user_id, y, rh, DM_NAV_NONE};
        y += rh + 2.0f;
        ui->dbg_relationship_rows++;
      }
    }
  }
  ui->chan_content_h = y + 8.0f;
  if (ui->capcrow > 256u && ui->ncrow * 4u < ui->capcrow) {
    size_t ncap = 64u;
    while (ncap < ui->ncrow * 2u) ncap *= 2u;
    if (ncap < ui->capcrow) {
      chan_row *shrunk = ccord_realloc_array(ui->crow, ncap, sizeof *shrunk);
      if (shrunk) {
        ui->crow = shrunk;
        ui->capcrow = ncap;
      }
    }
  }
}

typedef struct msort {
  size_t idx;
  bool offline;
  int32_t rank;   // lower is listed first
  const char *role;
  ccord_id role_id;   // the role it names, when known
  bool plain;         // no hoisted role: listed under "Online"
  const char *name;
  uint32_t name_color;
} msort;

static int cmp_msort(const void *pa, const void *pb)
{
  const msort *a = pa, *b = pb;
  if (a->offline != b->offline) return a->offline ? 1 : -1;
  if (!a->offline) {
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    int c = strcmp(a->role, b->role);
    if (c != 0) return c;
  }
  int c = strcasecmp(a->name, b->name);
  if (c != 0) return c;
  return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static bool grow_mrow(ccord_ui *ui, size_t need)
{
  if (need <= ui->capmrow) return true;
  size_t ncap = ui->capmrow ? ui->capmrow : 64;
  while (ncap < need) ncap *= 2;
  member_row *n = ccord_realloc_array(ui->mrow, ncap, sizeof *n);
  if (!n) return false;
  ui->mrow = n;
  ui->capmrow = ncap;
  return true;
}

// What a group header says. The list is loaded a piece at a time, so the members held of a group can be fewer than the
// group has: the server's own count is printed when it is known and larger (as Discord prints it), else what is held.
static size_t member_group_shown(const ccord_ui *ui, ccord_id guild, const msort *first, size_t held)
{
  uint32_t told = 0;
  bool known;
  if (first->offline) known = ccord_store_member_group_size(ui->d.store, guild, CCORD_MEMBER_GROUP_OFFLINE, CCORD_ID_NONE, &told);
  else if (first->plain) known = ccord_store_member_group_size(ui->d.store, guild, CCORD_MEMBER_GROUP_ONLINE, CCORD_ID_NONE, &told);
  else if (first->role_id != CCORD_ID_NONE) known = ccord_store_member_group_size(ui->d.store, guild, CCORD_MEMBER_GROUP_ROLE, first->role_id, &told);
  else known = false;
  return known && told > held ? told : held;
}

static void shrink_mrow_if_needed(ccord_ui *ui)
{
  if (ui->capmrow > 256u && ui->nmrow * 4u < ui->capmrow) {
    size_t ncap = 64u;
    while (ncap < ui->nmrow * 2u) ncap *= 2u;
    if (ncap < ui->capmrow) {
      member_row *shrunk = ccord_realloc_array(ui->mrow, ncap, sizeof *shrunk);
      if (shrunk) {
        ui->mrow = shrunk;
        ui->capmrow = ncap;
      }
    }
  }
}

static void rebuild_member_rows(ccord_ui *ui)
{
  ui->nmrow = 0;
  ui->member_content_h = 0.0f;
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  size_t n = ccord_store_member_count(ui->d.store, g);
  if (n == 0) {
    shrink_mrow_if_needed(ui);
    return;
  }
  msort *tmp = ccord_alloc_array(n, sizeof *tmp);
  if (!tmp) return;
  for (size_t i = 0; i < n; i++) {
    const ccord_member *m = ccord_store_member_at(ui->d.store, g, i);
    const ccord_user *u = ccord_store_user(ui->d.store, m->user_id);
    ui_member_look look;
    ui_member_look_of(ui, g, m, &look);
    const bool plain = !look.group || look.group[0] == '\0';
    const char *rname = plain ? "Online" : look.group;   // Discord: no hoisted role, "Online"
    int32_t rank = plain ? 2000 : 1000 - look.group_position;
    const char *mname = (m->nick[0] != '\0') ? m->nick : (u ? u->name : "");
    tmp[i] = (msort){i, !u || u->presence == CCORD_PRESENCE_OFFLINE, rank, rname,
                     plain ? CCORD_ID_NONE : look.group_id, plain, mname, look.color};
  }
  qsort(tmp, n, sizeof *tmp, cmp_msort);
  float y = 8.0f;
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && tmp[j].offline == tmp[i].offline && (tmp[i].offline || strcmp(tmp[j].role, tmp[i].role) == 0)) j++;
    if (!grow_mrow(ui, ui->nmrow + 1 + (j - i))) break;
    member_row *h = &ui->mrow[ui->nmrow++];
    *h = (member_row){.is_header = true, .y = y, .h = M(member_group_height)};
    const char *role = tmp[i].offline ? "Offline" : tmp[i].role;
    size_t k = 0;
    for (; role[k] && k < sizeof h->label - 24; k++) h->label[k] = (char)toupper((unsigned char)role[k]);
    h->label[k] = '\0';
    size_t used = strlen(h->label);
    snprintf(h->label + used, sizeof h->label - used, " \xE2\x80\x94 %zu", member_group_shown(ui, g, &tmp[i], j - i));
    y += h->h;
    for (size_t k2 = i; k2 < j; k2++) {
      ui->mrow[ui->nmrow++] = (member_row){
        .is_header = false,
        .index = tmp[k2].idx,
        .y = y,
        .h = M(member_item_height),
        .name_color = tmp[k2].name_color,
      };
      y += M(member_item_height);
    }
    i = j;
  }
  ccord_free(tmp);
  ui->member_content_h = y + 8.0f;
  shrink_mrow_if_needed(ui);
}

uint64_t uc_sig_mix_u64(uint64_t h, uint64_t v)
{
  return (h ^ v) * 1099511628211ull;
}

static uint64_t sig_mix_str(uint64_t h, const char *s)
{
  if (!s) return uc_sig_mix_u64(h, 0u);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h = (h ^ (uint64_t)*p) * 1099511628211ull;
  }
  return uc_sig_mix_u64(h, 0xFFu);
}

static uint64_t compute_chan_sig(const ccord_ui *ui, ccord_id g)
{
  const ccord_store *st = ui->d.store;
  size_t n = ccord_store_channel_count(st, g);
  ccord_id sel = ccord_store_selected_channel(st);
  uint64_t h = 14695981039346656037ull;
  h = uc_sig_mix_u64(h, g);
  h = uc_sig_mix_u64(h, (uint64_t)n);
  h = uc_sig_mix_u64(h, sel);
  h = uc_sig_mix_u64(h, (uint64_t)(uintptr_t)ui->theme);
  uint32_t zb = 0;
  memcpy(&zb, &ui->settings_zoom, sizeof zb);
  h = uc_sig_mix_u64(h, zb);
  if (g == CCORD_DM_GUILD_ID) {
    h = uc_sig_mix_u64(h, ui->dm_requests_view ? 1u : 0u);
    h = uc_sig_mix_u64(h, (uint64_t)ui->npinned_dms);
    for (size_t i = 0; i < ui->npinned_dms; i++) h = uc_sig_mix_u64(h, ui->pinned_dms[i]);
    h = uc_sig_mix_u64(h, (uint64_t)ui->nfavorite_dms);
    for (size_t i = 0; i < ui->nfavorite_dms; i++) h = uc_sig_mix_u64(h, ui->favorite_dms[i]);
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *ch = ccord_store_channel_at(st, g, i);
      if (!ch) continue;
      h = uc_sig_mix_u64(h, ch->id);
      h = uc_sig_mix_u64(h, ((uint64_t)ch->kind << 16) ^ (ch->is_message_request ? 1u : 0u) ^ (ch->thread_archived ? 2u : 0u));
      h = uc_sig_mix_u64(h, ch->parent_id);
      h = uc_sig_mix_u64(h, ch->last_message_id);
    }
    size_t nrel = ccord_store_relationship_count(st);
    h = uc_sig_mix_u64(h, (uint64_t)nrel);
    for (size_t i = 0; i < nrel; i++) {
      const ccord_relationship *rel = ccord_store_relationship_at(st, i);
      if (!rel) continue;
      h = uc_sig_mix_u64(h, rel->user_id);
      h = uc_sig_mix_u64(h, (uint64_t)rel->kind);
    }
    return h ? h : 1ull;
  }
  h = uc_sig_mix_u64(h, (uint64_t)ui->ncollapsed);
  for (size_t i = 0; i < ui->ncollapsed; i++) h = uc_sig_mix_u64(h, ui->collapsed[i]);
  h = uc_sig_mix_u64(h, (uint64_t)ui->nhide_muted_guilds);
  for (size_t i = 0; i < ui->nhide_muted_guilds; i++) h = uc_sig_mix_u64(h, ui->hide_muted_guilds[i]);
  h = uc_sig_mix_u64(h, (uint64_t)ui->npinned_channels);
  for (size_t i = 0; i < ui->npinned_channels; i++) h = uc_sig_mix_u64(h, ui->pinned_channels[i]);
  h = uc_sig_mix_u64(h, (uint64_t)ui->nhidden_voice_names);
  for (size_t i = 0; i < ui->nhidden_voice_names; i++) h = uc_sig_mix_u64(h, ui->hidden_voice_names[i]);
  bool preview = ui_role_preview_active_for(ui, g);
  ccord_id self = ccord_store_self_id(st);
  bool exact = ccord_store_permissions_exact(st, g, self);
  h = uc_sig_mix_u64(h, (preview ? 1u : 0u) | (exact ? 2u : 0u));
  if (preview) h = uc_sig_mix_u64(h, ui->role_preview_role_id);
  if (preview || exact) {
    const ccord_guild *gd = ccord_store_guild(st, g);
    h = uc_sig_mix_u64(h, gd ? gd->owner_id : 0u);
    size_t nr = 0;
    const ccord_role *roles = ccord_store_roles(st, g, &nr);
    h = uc_sig_mix_u64(h, (uint64_t)nr);
    for (size_t r = 0; roles && r < nr; r++) {
      h = uc_sig_mix_u64(h, roles[r].id);
      h = uc_sig_mix_u64(h, roles[r].permissions);
    }
    if (exact) {
      const ccord_id *srids = nullptr;
      size_t nsr = 0;
      if (ccord_store_member_roles(st, g, self, &srids, &nsr)) {
        h = uc_sig_mix_u64(h, (uint64_t)nsr + 1u);
        for (size_t r = 0; r < nsr; r++) h = uc_sig_mix_u64(h, srids[r]);
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_channel *ch = ccord_store_channel_at(st, g, i);
    if (!ch) continue;
    h = uc_sig_mix_u64(h, ch->id);
    h = uc_sig_mix_u64(h, ((uint64_t)ch->kind << 24) ^ ((uint64_t)ch->discord_type << 8) ^
                         (ch->thread_archived ? 1u : 0u) ^ (ccord_store_channel_is_forum(st, ch->id) ? 2u : 0u));
    h = uc_sig_mix_u64(h, ch->parent_id);
    if (ch->kind == CCORD_CHANNEL_VOICE || ch->kind == CCORD_CHANNEL_STAGE) {
      size_t nv = ccord_store_voice_count(st, ch->id);
      h = uc_sig_mix_u64(h, (uint64_t)nv);
      for (size_t v = 0; v < nv; v++) {
        const ccord_voice_state *vs = ccord_store_voice_at(st, ch->id, v);
        if (vs) h = uc_sig_mix_u64(h, vs->user_id);
      }
    }
    if (preview || exact) {
      size_t now = 0;
      const ccord_overwrite *ow = ccord_store_channel_overwrites(st, ch->id, &now);
      h = uc_sig_mix_u64(h, (uint64_t)now);
      for (size_t w = 0; ow && w < now; w++) {
        h = uc_sig_mix_u64(h, ow[w].id);
        h = uc_sig_mix_u64(h, ow[w].allow);
        h = uc_sig_mix_u64(h, ow[w].deny);
      }
    }
  }
  return h ? h : 1ull;
}

static uint64_t compute_member_sig(const ccord_ui *ui, ccord_id g)
{
  const ccord_store *st = ui->d.store;
  size_t n = ccord_store_member_count(st, g);
  if (n == 0) return 1ull;
  uint64_t h = 14695981039346656037ull;
  h = uc_sig_mix_u64(h, g);
  h = uc_sig_mix_u64(h, (uint64_t)n);
  h = uc_sig_mix_u64(h, (uint64_t)(uintptr_t)ui->theme);
  uint32_t zb = 0;
  memcpy(&zb, &ui->settings_zoom, sizeof zb);
  h = uc_sig_mix_u64(h, zb);
  uint32_t gsz = 0;
  if (ccord_store_member_group_size(st, g, CCORD_MEMBER_GROUP_ONLINE, CCORD_ID_NONE, &gsz)) {
    h = uc_sig_mix_u64(h, (uint64_t)gsz + 1u);
  }
  if (ccord_store_member_group_size(st, g, CCORD_MEMBER_GROUP_OFFLINE, CCORD_ID_NONE, &gsz)) {
    h = uc_sig_mix_u64(h, ((uint64_t)gsz + 1u) << 16);
  }
  size_t nr = 0;
  const ccord_role *roles = ccord_store_roles(st, g, &nr);
  h = uc_sig_mix_u64(h, (uint64_t)nr);
  for (size_t r = 0; roles && r < nr; r++) {
    h = uc_sig_mix_u64(h, roles[r].id);
    h = uc_sig_mix_u64(h, ((uint64_t)(uint32_t)roles[r].position << 32) ^ (uint64_t)roles[r].color ^ (roles[r].hoist ? 0x80000000ull : 0ull));
    if (roles[r].hoist) {
      h = sig_mix_str(h, roles[r].name);
      if (ccord_store_member_group_size(st, g, CCORD_MEMBER_GROUP_ROLE, roles[r].id, &gsz)) {
        h = uc_sig_mix_u64(h, (uint64_t)gsz + 1u);
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_member *m = ccord_store_member_at(st, g, i);
    if (!m) continue;
    const ccord_user *u = ccord_store_user(st, m->user_id);
    h = uc_sig_mix_u64(h, m->user_id);
    h = uc_sig_mix_u64(h, ((uint64_t)m->role_color << 16) ^ ((uint64_t)m->role_rank << 8) ^ (u ? (uint64_t)u->presence : 0xFFu));
    if (m->nick[0]) h = sig_mix_str(h, m->nick);
    else h = sig_mix_str(h, u ? u->name : nullptr);
    if (m->role_name && m->role_name[0]) h = sig_mix_str(h, m->role_name);
    const ccord_id *mrids = nullptr;
    size_t nmr = 0;
    if (ccord_store_member_roles(st, g, m->user_id, &mrids, &nmr)) {
      h = uc_sig_mix_u64(h, (uint64_t)nmr + 1u);
      for (size_t k = 0; k < nmr; k++) h = uc_sig_mix_u64(h, mrids[k]);
    }
  }
  return h ? h : 1ull;
}

// UI-side state that the rows depend on changed (a category was collapsed, a conversation opened): rebuild them next time.
void ui_invalidate_rows(ccord_ui *ui)
{
  ui->built_rev = 0;
  ui->built_lists_rev = UINT64_MAX;   // no store revision is this high
  ui->built_chan_sig = 0;
  ui->built_member_sig = 0;
}

void ui_start_role_preview(ccord_ui *ui, ccord_id guild_id, ccord_id role_id)
{
  if (!ui || !ui->d.store || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return;
  if (!ccord_store_roles_known(ui->d.store, guild_id)) {
    ui_notice(ui, "Roles for this server are not loaded yet.");
    return;
  }
  if (ccord_store_selected_guild(ui->d.store) != guild_id) {
    ccord_error err;
    if (ui->d.session) (void)ccord_session_select_guild(ui->d.session, guild_id, &err);
    else (void)ccord_store_select_guild(ui->d.store, guild_id, &err);
  }
  ui->role_preview_guild = guild_id;
  ui->role_preview_role_id = (role_id != CCORD_ID_NONE && ccord_store_role(ui->d.store, guild_id, role_id))
                               ? role_id
                               : guild_id;
  ui_invalidate_rows(ui);
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

void ui_stop_role_preview(ccord_ui *ui)
{
  if (!ui || ui->role_preview_guild == CCORD_ID_NONE) return;
  ui->role_preview_guild = CCORD_ID_NONE;
  ui->role_preview_role_id = CCORD_ID_NONE;
  ui->role_preview_cycle_rect = (ccord_rect){0};
  ui->role_preview_exit_rect = (ccord_rect){0};
  ui->update_banner_install_rect = (ccord_rect){0};
  ui->update_banner_notes_rect = (ccord_rect){0};
  ui->update_banner_dismiss_rect = (ccord_rect){0};
  ui_invalidate_rows(ui);
  ui_compute_layout(ui);
  ccord_vlist_invalidate_all(&ui->vlist);
  ui->dirty = true;
}

void ui_cycle_role_preview(ccord_ui *ui)
{
  if (!ui || !ui_role_preview_active_for(ui, ui->role_preview_guild)) return;
  size_t nroles = 0;
  const ccord_role *roles = ccord_store_roles(ui->d.store, ui->role_preview_guild, &nroles);
  if (!roles || nroles == 0) return;
  size_t idx = 0;
  for (size_t i = 0; i < nroles; i++) {
    if (roles[i].id == ui->role_preview_role_id) {
      idx = (i + 1u) % nroles;
      break;
    }
  }
  ui->role_preview_role_id = roles[idx].id;
  ui_invalidate_rows(ui);
  ui->dirty = true;
}

void ui_ensure_caches(ccord_ui *ui)
{
  ui_member_verification_sync(ui);   // a pending guild opens its rules form; a cleared one closes it
  if (ui->role_preview_guild != CCORD_ID_NONE) {
    ccord_id cur_g = ccord_store_selected_guild(ui->d.store);
    if (cur_g != ui->role_preview_guild || !ccord_store_guild(ui->d.store, ui->role_preview_guild) ||
        !ccord_store_roles_known(ui->d.store, ui->role_preview_guild)) {
      ui_stop_role_preview(ui);
    } else if (ui->role_preview_role_id != ui->role_preview_guild &&
               !ccord_store_role(ui->d.store, ui->role_preview_guild, ui->role_preview_role_id)) {
      ui->role_preview_role_id = ui->role_preview_guild;
      ui_invalidate_rows(ui);
    }
  }
  if (ui->pending_dm_user != CCORD_ID_NONE) {
    size_t ndm = ccord_store_channel_count(ui->d.store, CCORD_DM_GUILD_ID);
    for (size_t i = 0; i < ndm; i++) {
      const ccord_channel *c = ccord_store_channel_at(ui->d.store, CCORD_DM_GUILD_ID, i);
      if (!c) continue;
      if (c->user_id == ui->pending_dm_user || c->id == 900000u + ui->pending_dm_user) {
        ui->pending_dm_user = CCORD_ID_NONE;
        ccord_error err;
        if (ui->d.session) (void)ccord_session_select_channel(ui->d.session, c->id, &err);
        else (void)ccord_store_select_channel(ui->d.store, c->id, &err);
        break;
      }
    }
  }
  if (ui->discovery_pending_guild_id != CCORD_ID_NONE &&
      ccord_store_guild(ui->d.store, ui->discovery_pending_guild_id) != nullptr) {
    ui->discovery_pending_guild_id = CCORD_ID_NONE;
    if (ui->discovery_open) {
      ui->discovery_open = false;
      ui_compute_layout(ui);
    }
  }
  // Selecting a 1:1 DM, a group DM or the friends view swaps which panel (and which hidden flag) the right column
  // follows without changing the guild: lay out again when that pair moved.
  if (ui_panel_state(ui) != ui->panel_state) {
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
    ui->dirty = true;
  }
  uint64_t rev = ccord_store_revision(ui->d.store);
  uint64_t lists_rev = ccord_store_lists_revision(ui->d.store);
  ccord_id g = ccord_store_selected_guild(ui->d.store);
  if (rev == ui->built_rev && g == ui->built_guild) return;
  bool had_built_guild = (ui->built_guild != CCORD_ID_NONE);
  ui->built_rev = rev;
  bool guild_changed = g != ui->built_guild;
  if (guild_changed) {
    if (had_built_guild && ui->discovery_open) {
      ui->discovery_open = false;
    }
    ui->chan_scroll = 0.0f;
    ui->member_scroll = 0.0f;
    ui->chan_scroll_vel = 0.0f;
    ui->member_scroll_vel = 0.0f;
    ui->built_guild = g;
    ui_compute_layout(ui);
    ccord_vlist_invalidate_all(&ui->vlist);
  }
  // The lists of channels and members are rebuilt (and the members sorted) only when something other than message traffic
  // changed: a message, a reaction or someone typing needs a redraw, not new lists. Channel and member caches are
  // fingerprinted independently so a presence change skips channel rebuild and a channel edit skips member sort.
  if (guild_changed || lists_rev != ui->built_lists_rev) {
    ui->built_lists_rev = lists_rev;
    if ((ui_self_voice(ui) != nullptr) != ui->built_voice_bar) ui_compute_layout(ui);
    uint64_t csig = compute_chan_sig(ui, g);
    uint64_t msig = compute_member_sig(ui, g);
    bool did_rebuild = false;
    if (guild_changed || csig != ui->built_chan_sig) {
      rebuild_channel_rows(ui);
      ui->built_chan_sig = csig;
      did_rebuild = true;
    }
    if (guild_changed || msig != ui->built_member_sig) {
      rebuild_member_rows(ui);
      ui->built_member_sig = msig;
      did_rebuild = true;
    }
    if (did_rebuild) ui->dbg_list_rebuilds++;
  }
  ui->dirty = true;
}
