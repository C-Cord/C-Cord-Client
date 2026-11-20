// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

bool mock_admin_server_guild(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  static const char unknown_role[] = "Unknown Role";
  static const char unknown_member[] = "Unknown Member";
  ccord_id g = req->guild_id;
  *out = CCORD_OK;
  switch (req->op) {
    case CCORD_ADMIN_CREATE_ROLE: {
      size_t cnt = 0, first = mock_guild_roles(m, g, &cnt);
      if (cnt == 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      uint64_t perms = req->role_permissions >= 0 ? (uint64_t)req->role_permissions : m->roles[first].permissions;
      if (!mock_may(m, g, CCORD_PERM_MANAGE_ROLES) || m->nroles == MOCK_ROLES_MAX || cnt > CCORD_MAX_ROLES_PER_GUILD ||
          ccord_perm_can_grant(g, mock_guild_owner(m, g), m->self_id, mock_self_perms(m, g), 0, perms) !=
            CCORD_PERM_ALLOWED) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *name = mk_admin_owned(m, req->name[0] != '\0' ? req->name : "new role");
      if (!name) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      ccord_id ids[CCORD_ADMIN_ROLE_REORDER_MAX];
      size_t nids = 1;
      for (size_t i = 0; i < m->nroles; i++) {   // the new role sits just above @everyone; the rest move up one
        if (m->roles[i].guild_id == g && m->roles[i].id != g) {
          m->roles[i].position++;
          if (nids < CCORD_ADMIN_ROLE_REORDER_MAX) ids[nids++] = m->roles[i].id;
        }
      }
      ccord_id id = m->next_role_id++;
      ids[0] = id;
      size_t at = first + cnt;   // the end of the guild's group keeps the table grouped
      memmove(&m->roles[at + 1], &m->roles[at], (m->nroles - at) * sizeof m->roles[0]);
      m->roles[at] = (ccord_role){.id = id, .guild_id = g, .name = name,
                                  .color = req->role_color >= 0 ? (uint32_t)req->role_color : 0u, .position = 1,
                                  .permissions = perms, .hoist = req->role_hoist > 0,
                                  .mentionable = req->role_mentionable > 0};
      m->nroles++;
      *out = mock_audit_add(m, g, 30, id, name, req->reason, name);
      if (*out == CCORD_OK) *out = mk_admin_emit_roles(m, CCORD_NET_ROLE_UPSERT, ids, nids, g, err);
      return true;
    }
    case CCORD_ADMIN_EDIT_ROLE: {
      size_t ri = mock_role_find(m, g, req->role_id);
      if (ri == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
        return true;
      }
      uint64_t old = m->roles[ri].permissions;
      uint64_t now = req->role_permissions >= 0 ? (uint64_t)req->role_permissions : old;
      if (!mock_may_edit_role(m, g, req->role_id, false) ||
          ccord_perm_can_grant(g, mock_guild_owner(m, g), m->self_id, mock_self_perms(m, g), old, now) !=
            CCORD_PERM_ALLOWED) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char detail[128];
      detail[0] = '\0';
      if (req->name[0] != '\0') {
        const char *name = mk_admin_owned(m, req->name);
        if (!name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        (void)snprintf(detail, sizeof detail, "name: %.40s -> %.40s", m->roles[ri].name, name);
        m->roles[ri].name = name;
      }
      if (req->role_permissions >= 0) {
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "permissions changed");
        m->roles[ri].permissions = now;
      }
      if (req->role_color >= 0) {
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "color: %u -> %d", m->roles[ri].color, req->role_color);
        m->roles[ri].color = (uint32_t)req->role_color;
      }
      if (req->role_hoist >= 0) {
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "hoist: %s", req->role_hoist ? "yes" : "no");
        m->roles[ri].hoist = req->role_hoist != 0;
      }
      if (req->role_mentionable >= 0) {
        if (detail[0] == '\0') {
          (void)snprintf(detail, sizeof detail, "mentionable: %s", req->role_mentionable ? "yes" : "no");
        }
        m->roles[ri].mentionable = req->role_mentionable != 0;
      }
      for (size_t i = 0; i < m->nmembers; i++) {   // names and colours shown in the member list follow the role
        if (m->members[i].guild_id == g) mock_refresh_display(m, g, m->members[i].user_id);
      }
      *out = mock_audit_add(m, g, 31, req->role_id, m->roles[ri].name, req->reason, detail);
      if (*out == CCORD_OK) *out = mk_admin_emit_roles(m, CCORD_NET_ROLE_UPSERT, &req->role_id, 1, g, err);
      return true;
    }
    case CCORD_ADMIN_DELETE_ROLE: {
      size_t ri = mock_role_find(m, g, req->role_id);
      if (ri == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
        return true;
      }
      if (!mock_may_edit_role(m, g, req->role_id, false)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      *out = mock_audit_add(m, g, 32, req->role_id, m->roles[ri].name, req->reason, "");
      memmove(&m->roles[ri], &m->roles[ri + 1], (m->nroles - ri - 1) * sizeof m->roles[0]);
      m->nroles--;
      for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
        mock_mrow *r = &m->mrows[i];
        if (!r->present || r->guild_id != g) continue;
        for (size_t k = 0; k < r->count; k++) {
          if (r->ids[k] != req->role_id) continue;
          memmove(&r->ids[k], &r->ids[k + 1], (r->count - k - 1) * sizeof r->ids[0]);
          r->count--;
          break;
        }
        mock_refresh_display(m, g, r->user_id);
      }
      mock_sync_member_roles(m);
      if (*out != CCORD_OK) return true;
      mock_item it = mk_make_item(CCORD_NET_ROLE_DELETE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.role_id = req->role_id;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_REORDER_ROLES: {
      for (size_t i = 0; i < req->reorder_count; i++) {
        if (mock_role_find(m, g, req->reorder_ids[i]) == SIZE_MAX) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
          return true;
        }
        if (!mock_may_edit_role(m, g, req->reorder_ids[i], false)) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
          return true;
        }
      }
      for (size_t i = 0; i < req->reorder_count; i++) {
        m->roles[mock_role_find(m, g, req->reorder_ids[i])].position = req->reorder_positions[i];
      }
      for (size_t i = 0; i < m->nmembers; i++) {
        if (m->members[i].guild_id == g) mock_refresh_display(m, g, m->members[i].user_id);
      }
      size_t cnt = 0, first = mock_guild_roles(m, g, &cnt);
      ccord_id ids[CCORD_ADMIN_ROLE_REORDER_MAX];
      size_t n = 0;
      for (size_t i = 0; i < cnt && n < CCORD_ADMIN_ROLE_REORDER_MAX; i++) ids[n++] = m->roles[first + i].id;
      *out = mock_audit_add(m, g, 31, req->reorder_ids[0], m->roles[mock_role_find(m, g, req->reorder_ids[0])].name,
                            req->reason, "position changed");
      if (*out == CCORD_OK) *out = mk_admin_emit_roles(m, CCORD_NET_ROLE_UPSERT, ids, n, g, err);
      return true;
    }
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE: {
      bool add = req->op == CCORD_ADMIN_ADD_MEMBER_ROLE;
      if (mock_role_find(m, g, req->role_id) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_role, err);
        return true;
      }
      mock_mrow *r = mock_mrow_find(m, g, req->user_id);
      if (!r) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
        return true;
      }
      if (!mock_may_edit_role(m, g, req->role_id, true)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t at = SIZE_MAX;
      for (size_t k = 0; k < r->count; k++) {
        if (r->ids[k] == req->role_id) at = k;
      }
      if (add && at == SIZE_MAX) {
        if (r->count == MOCK_MEMBER_ROLES_MAX) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of roles reached", err);
          return true;
        }
        r->ids[r->count++] = req->role_id;
      } else if (!add && at != SIZE_MAX) {
        memmove(&r->ids[at], &r->ids[at + 1], (r->count - at - 1) * sizeof r->ids[0]);
        r->count--;
      }
      mock_refresh_display(m, g, req->user_id);
      const ccord_user *tu = nullptr;
      for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
        if (m->users[i].id == req->user_id) tu = &m->users[i];
      }
      *out = mock_audit_add(m, g, 25, req->user_id, tu ? tu->name : "", req->reason,
                            add ? "added a role" : "removed a role");
      if (*out == CCORD_OK) *out = mk_admin_emit_member_roles(m, g, req->user_id, req->role_id, add, err);
      return true;
    }
    case CCORD_ADMIN_UNBAN: {
      if (!mock_may(m, g, CCORD_PERM_BAN_MEMBERS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t at = SIZE_MAX;
      for (size_t i = 0; i < m->nbans; i++) {
        if (m->bans[i].guild_id == g && m->bans[i].ban.user_id == req->user_id) at = i;
      }
      if (at == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Ban", err);
        return true;
      }
      *out = mock_audit_add(m, g, 23, req->user_id, m->bans[at].ban.name, req->reason, "");
      memmove(&m->bans[at], &m->bans[at + 1], (m->nbans - at - 1) * sizeof m->bans[0]);
      m->nbans--;
      if (*out != CCORD_OK) return true;
      mock_item it = mk_make_item(CCORD_NET_BAN_REMOVE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.user_id = req->user_id;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_INVITE: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_CREATE_INSTANT_INVITE) || m->ninvites == MOCK_INVITES_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char code[CCORD_ADMIN_INVITE_CODE_MAX + 1];
      (void)snprintf(code, sizeof code, "mock%04u", m->next_invite++);
      const char *own = mk_admin_owned(m, code);
      if (!own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      int64_t now = (int64_t)mk_now_of(m);
      m->invites[m->ninvites++] = (mock_invite){
        g, {.code = own, .channel_id = req->channel_id, .channel_name = m->channels[ci].name, .inviter_id = m->self_id,
            .inviter_name = m->users[0].name, .max_uses = req->invite_max_uses, .max_age_s = req->invite_max_age_s,
            .temporary = req->invite_temporary, .created_ms = now,
            .expires_ms = req->invite_max_age_s ? now + (int64_t)req->invite_max_age_s * 1000 : 0}};
      *out = mock_audit_add(m, g, 40, 0, own, "", "invite created");
      if (*out == CCORD_OK) *out = mk_admin_emit_invites(m, CCORD_NET_INVITE_UPSERT, g, own, err);
      return true;
    }
    case CCORD_ADMIN_REVOKE_INVITE: {
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      size_t at = SIZE_MAX;
      for (size_t i = 0; i < m->ninvites; i++) {
        if (m->invites[i].guild_id == g && strcmp(m->invites[i].inv.code, req->code) == 0) at = i;
      }
      if (at == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Invite", err);
        return true;
      }
      const char *code = m->invites[at].inv.code;   // a literal or a retired string: stable until destroy
      *out = mock_audit_add(m, g, 42, 0, code, req->reason, "");
      memmove(&m->invites[at], &m->invites[at + 1], (m->ninvites - at - 1) * sizeof m->invites[0]);
      m->ninvites--;
      if (*out == CCORD_OK) *out = mk_admin_emit_invites(m, CCORD_NET_INVITE_DELETE, g, code, err);
      return true;
    }
    case CCORD_ADMIN_LIST_BANS:
      *out = mock_may(m, g, CCORD_PERM_BAN_MEMBERS)
               ? mk_admin_emit_bans(m, g, err)
               : mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      return true;
    case CCORD_ADMIN_LIST_INVITES:
      *out = mock_may(m, g, CCORD_PERM_MANAGE_GUILD)
               ? mk_admin_emit_invites(m, CCORD_NET_INVITE_LIST, g, nullptr, err)
               : mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      return true;
    case CCORD_ADMIN_LIST_AUDIT_LOG:
      *out = mock_may(m, g, CCORD_PERM_VIEW_AUDIT_LOG)
               ? mk_admin_emit_audit(m, req, err)
               : mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      return true;
    case CCORD_ADMIN_REORDER_CHANNELS: {
      if (!mock_guild_known(m, g)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_CHANNELS)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      for (size_t i = 0; i < req->reorder_count; i++) {
        size_t ci = mk_admin_channel_index(m, req->reorder_ids[i]);
        if (ci == SIZE_MAX || m->channels[ci].guild_id != g) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
          return true;
        }
        if (req->reorder_has_parent[i] && req->reorder_parents[i] != CCORD_ID_NONE) {
          size_t pci = mk_admin_channel_index(m, req->reorder_parents[i]);
          if (pci == SIZE_MAX || m->channels[pci].guild_id != g || m->channels[pci].kind != CCORD_CHANNEL_CATEGORY ||
              m->channels[ci].kind == CCORD_CHANNEL_CATEGORY) {
            *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid parent category", err);
            return true;
          }
        }
      }
      for (size_t i = 0; i < req->reorder_count; i++) {
        size_t ci = mk_admin_channel_index(m, req->reorder_ids[i]);
        m->channels[ci].position = req->reorder_positions[i];
        if (req->reorder_has_parent[i]) m->channels[ci].parent_id = req->reorder_parents[i];
      }
      size_t n = req->reorder_count;
      size_t first_ci = mk_admin_channel_index(m, req->reorder_ids[0]);
      *out = mock_audit_add(m, g, 11, req->reorder_ids[0],
                            first_ci != SIZE_MAX ? m->channels[first_ci].name : "", req->reason, "position changed");
      if (*out != CCORD_OK) return true;
      mock_item it = mk_make_item(CCORD_NET_CHANNEL_POSITIONS, mk_now_of(m) + m->cfg.latency_ms);
      it.reorder_ids_own = ccord_alloc_array(n, sizeof *it.reorder_ids_own);
      it.reorder_pos_own = ccord_alloc_array(n, sizeof *it.reorder_pos_own);
      it.reorder_par_own = ccord_alloc_array(n, sizeof *it.reorder_par_own);
      it.reorder_hpar_own = ccord_alloc_array(n, sizeof *it.reorder_hpar_own);
      if (!it.reorder_ids_own || !it.reorder_pos_own || !it.reorder_par_own || !it.reorder_hpar_own) {
        mk_item_release(&it);
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      memcpy(it.reorder_ids_own, req->reorder_ids, n * sizeof *it.reorder_ids_own);
      memcpy(it.reorder_pos_own, req->reorder_positions, n * sizeof *it.reorder_pos_own);
      memcpy(it.reorder_par_own, req->reorder_parents, n * sizeof *it.reorder_par_own);
      memcpy(it.reorder_hpar_own, req->reorder_has_parent, n * sizeof *it.reorder_hpar_own);
      it.ev.guild_id = g;
      it.ev.reorder_ids = it.reorder_ids_own;
      it.ev.reorder_positions = it.reorder_pos_own;
      it.ev.reorder_parents = it.reorder_par_own;
      it.ev.reorder_has_parent = it.reorder_hpar_own;
      it.ev.reorder_count = n;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_EDIT_GUILD: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      if (req->has_system_channel && req->system_channel_id != CCORD_ID_NONE) {
        size_t sci = mk_admin_channel_index(m, req->system_channel_id);
        if (sci == SIZE_MAX || m->channels[sci].guild_id != g || m->channels[sci].kind != CCORD_CHANNEL_TEXT) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid system channel", err);
          return true;
        }
      }
      if (req->has_afk_channel && req->afk_channel_id != CCORD_ID_NONE) {
        size_t aci = mk_admin_channel_index(m, req->afk_channel_id);
        if (aci == SIZE_MAX || m->channels[aci].guild_id != g || m->channels[aci].kind != CCORD_CHANNEL_VOICE ||
            m->channels[aci].discord_type == CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid AFK channel", err);
          return true;
        }
      }
      if (req->has_rules_channel && req->rules_channel_id != CCORD_ID_NONE) {
        size_t rci = mk_admin_channel_index(m, req->rules_channel_id);
        if (rci == SIZE_MAX || m->channels[rci].guild_id != g || m->channels[rci].kind != CCORD_CHANNEL_TEXT) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid rules channel", err);
          return true;
        }
      }
      if (req->has_public_updates_channel && req->public_updates_channel_id != CCORD_ID_NONE) {
        size_t puci = mk_admin_channel_index(m, req->public_updates_channel_id);
        if (puci == SIZE_MAX || m->channels[puci].guild_id != g || m->channels[puci].kind != CCORD_CHANNEL_TEXT) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid community updates channel", err);
          return true;
        }
      }
      ccord_guild *gd = &m->guilds[gi];
      ccord_audit_entry changes_tmp = {0};
      char detail[128] = "";
      if (req->has_guild_name || req->name[0] != '\0') {
        const char *old_n = gd->name ? gd->name : "";
        const char *name = mk_admin_owned(m, req->name);
        if (!name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        mock_audit_push_change(&changes_tmp, "name", old_n, name);
        (void)snprintf(detail, sizeof detail, "name: %.40s -> %.40s", old_n, name);
        gd->name = name;
      }
      if (req->has_guild_icon) {
        const char *old_icon = gd->icon_url ? gd->icon_url : "";
        if (req->clear_guild_icon) {
          gd->icon_url = "";
          mock_audit_push_change(&changes_tmp, "icon_hash", old_icon, "");
          if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "icon_hash: cleared");
        } else {
          char url[96];
          (void)snprintf(url, sizeof url, "https://cdn.discordapp.com/icons/%llu/icon_%zu.png",
                         (unsigned long long)g, m->admin_count);
          const char *owned_url = mk_admin_owned(m, url);
          if (!owned_url) {
            *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
            return true;
          }
          gd->icon_url = owned_url;
          mock_audit_push_change(&changes_tmp, "icon_hash", old_icon, owned_url);
          if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "icon_hash: updated");
        }
      }
      if (req->has_guild_banner) {
        char old_b[256];
        memcpy(old_b, gd->banner_url, sizeof old_b);
        if (req->clear_guild_banner) {
          gd->banner_url[0] = '\0';
          mock_audit_push_change(&changes_tmp, "banner_hash", old_b, "");
          if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "banner_hash: cleared");
        } else {
          (void)snprintf(gd->banner_url, sizeof gd->banner_url,
                         "https://cdn.discordapp.com/banners/%llu/banner_%zu.png",
                         (unsigned long long)g, m->admin_count);
          mock_audit_push_change(&changes_tmp, "banner_hash", old_b, gd->banner_url);
          if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "banner_hash: updated");
        }
      }
      if (req->has_system_channel) {
        char ob[32], nb[32];
        ccord_id_format(gd->system_channel_id, ob);
        ccord_id_format(req->system_channel_id, nb);
        mock_audit_push_change(&changes_tmp, "system_channel_id", gd->system_channel_id ? ob : "",
                               req->system_channel_id ? nb : "");
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "system_channel_id: %s -> %s", ob, nb);
        gd->system_channel_id = req->system_channel_id;
      }
      if (req->guild_system_flags >= 0) {
        char ob[24], nb[24];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->system_channel_flags);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_system_flags);
        mock_audit_push_change(&changes_tmp, "system_channel_flags", ob, nb);
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "system_channel_flags: %s -> %s", ob, nb);
        gd->system_channel_flags = (uint32_t)req->guild_system_flags;
      }
      if (req->guild_default_notify >= 0) {
        char ob[16], nb[16];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->default_message_notifications);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_default_notify);
        mock_audit_push_change(&changes_tmp, "default_message_notifications", ob, nb);
        if (detail[0] == '\0') {
          (void)snprintf(detail, sizeof detail, "default_message_notifications: %s -> %s", ob, nb);
        }
        gd->default_message_notifications = (uint8_t)req->guild_default_notify;
      }
      if (req->has_afk_channel) {
        char ob[32], nb[32];
        ccord_id_format(gd->afk_channel_id, ob);
        ccord_id_format(req->afk_channel_id, nb);
        mock_audit_push_change(&changes_tmp, "afk_channel_id", gd->afk_channel_id ? ob : "",
                               req->afk_channel_id ? nb : "");
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "afk_channel_id: %s -> %s", ob, nb);
        gd->afk_channel_id = req->afk_channel_id;
      }
      if (req->guild_afk_timeout >= 0) {
        char ob[24], nb[24];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->afk_timeout);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_afk_timeout);
        mock_audit_push_change(&changes_tmp, "afk_timeout", ob, nb);
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "afk_timeout: %s -> %s", ob, nb);
        gd->afk_timeout = (uint32_t)req->guild_afk_timeout;
      }
      if (req->guild_verification_level >= 0) {
        char ob[16], nb[16];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->verification_level);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_verification_level);
        mock_audit_push_change(&changes_tmp, "verification_level", ob, nb);
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "verification_level: %s -> %s", ob, nb);
        gd->verification_level = (uint8_t)req->guild_verification_level;
      }
      if (req->guild_explicit_filter >= 0) {
        char ob[16], nb[16];
        (void)snprintf(ob, sizeof ob, "%u", (unsigned)gd->explicit_content_filter);
        (void)snprintf(nb, sizeof nb, "%u", (unsigned)req->guild_explicit_filter);
        mock_audit_push_change(&changes_tmp, "explicit_content_filter", ob, nb);
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "explicit_content_filter: %s -> %s", ob, nb);
        gd->explicit_content_filter = (uint8_t)req->guild_explicit_filter;
      }
      if (req->has_community) {
        const char *of = gd->feature_community ? "COMMUNITY" : "";
        const char *nf = req->feature_community ? "COMMUNITY" : "";
        mock_audit_push_change(&changes_tmp, "features", of, nf);
        if (detail[0] == '\0') {
          (void)snprintf(detail, sizeof detail, "features: %s", req->feature_community ? "+COMMUNITY" : "-COMMUNITY");
        }
        gd->feature_community = req->feature_community;
      }
      if (req->has_rules_channel) {
        char ob[32], nb[32];
        ccord_id_format(gd->rules_channel_id, ob);
        ccord_id_format(req->rules_channel_id, nb);
        mock_audit_push_change(&changes_tmp, "rules_channel_id", gd->rules_channel_id ? ob : "",
                               req->rules_channel_id ? nb : "");
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "rules_channel_id: %s -> %s", ob, nb);
        gd->rules_channel_id = req->rules_channel_id;
      }
      if (req->has_public_updates_channel) {
        char ob[32], nb[32];
        ccord_id_format(gd->public_updates_channel_id, ob);
        ccord_id_format(req->public_updates_channel_id, nb);
        mock_audit_push_change(&changes_tmp, "public_updates_channel_id", gd->public_updates_channel_id ? ob : "",
                               req->public_updates_channel_id ? nb : "");
        if (detail[0] == '\0') (void)snprintf(detail, sizeof detail, "public_updates_channel_id: %s -> %s", ob, nb);
        gd->public_updates_channel_id = req->public_updates_channel_id;
      }
      gd->has_guild_settings = true;
      *out = mock_audit_add(m, g, 1, g, gd->name, req->reason, detail[0] ? detail : "server updated");
      if (*out == CCORD_OK && changes_tmp.change_count > 0 && m->naudit > 0) {
        m->audit[m->naudit - 1].e.change_count = changes_tmp.change_count;
        memcpy(m->audit[m->naudit - 1].e.changes, changes_tmp.changes, sizeof changes_tmp.changes);
      }
      if (*out == CCORD_OK) *out = mock_emit_guild_upsert(m, gi, req->op, err);
      return true;
    }
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_guild *gd = &m->guilds[gi];
      gd->has_incidents_data = true;
      gd->invites_disabled_until_ms = req->invites_disabled_until_ms;
      gd->dms_disabled_until_ms = req->dms_disabled_until_ms;
      const char *det = req->invites_disabled_until_ms > 0 ? "invites_disabled_until: paused"
                      : req->dms_disabled_until_ms > 0     ? "dms_disabled_until: paused"
                                                           : "invites_disabled_until: active";
      *out = mock_audit_add(m, g, 1, g, gd->name, req->reason, det);
      if (*out != CCORD_OK) return true;
      mock_item it = mk_make_item(CCORD_NET_GUILD_INCIDENTS, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.invites_disabled_until_ms = req->invites_disabled_until_ms;
      it.ev.dms_disabled_until_ms = req->dms_disabled_until_ms;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_SET_VANITY_URL: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!mock_may(m, g, CCORD_PERM_MANAGE_GUILD)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_guild *gd = &m->guilds[gi];
      if (!gd->feature_vanity_url && gd->premium_tier < 3u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION,
                                  "Missing Access (Vanity URL requires Server Boost Level 3)", err);
        return true;
      }
      char old_code[64];
      memcpy(old_code, gd->vanity_url_code, sizeof old_code);
      (void)snprintf(gd->vanity_url_code, sizeof gd->vanity_url_code, "%s", req->code);
      const char *own_code = mk_admin_owned(m, req->code);
      if (!own_code) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      char det[160];
      (void)snprintf(det, sizeof det, "vanity_url_code: %s -> %s", old_code, req->code);
      *out = mock_audit_add(m, g, 1, g, gd->name, req->reason, det);
      if (*out != CCORD_OK) return true;
      mock_item it = mk_make_item(CCORD_NET_GUILD_VANITY, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.invite_code = own_code;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_LEAVE_GUILD: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (m->guilds[gi].owner_id != CCORD_ID_NONE && m->guilds[gi].owner_id == m->self_id) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid Guild (Owner cannot leave guild)", err);
        return true;
      }
      mock_remove_guild(m, gi);
      mock_item it = mk_make_item(CCORD_NET_GUILD_DELETE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.admin_op = req->op;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_DELETE_GUILD: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (m->guilds[gi].owner_id == CCORD_ID_NONE || m->guilds[gi].owner_id != m->self_id) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, "Only the server owner can delete this server", err);
        return true;
      }
      mock_remove_guild(m, gi);
      mock_item it = mk_make_item(CCORD_NET_GUILD_DELETE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.admin_op = req->op;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_TRANSFER_OWNERSHIP: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (m->guilds[gi].owner_id == CCORD_ID_NONE || m->guilds[gi].owner_id != m->self_id) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, "Only the server owner can transfer ownership", err);
        return true;
      }
      if (req->user_id == CCORD_ID_NONE || req->user_id == m->self_id || mk_admin_member_index(m, g, req->user_id) < 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, unknown_member, err);
        return true;
      }
      char ob[32], nb[32], det[96];
      ccord_id_format(m->guilds[gi].owner_id, ob);
      ccord_id_format(req->user_id, nb);
      (void)snprintf(det, sizeof det, "owner_id: %s -> %s", ob, nb);
      m->guilds[gi].owner_id = req->user_id;
      m->guilds[gi].self_permissions = mock_self_perms(m, g);
      m->guilds[gi].has_guild_settings = true;
      *out = mock_audit_add(m, g, 1, g, m->guilds[gi].name, req->reason, det);
      if (*out == CCORD_OK) *out = mock_emit_guild_upsert(m, gi, req->op, err);
      return true;
    }
    case CCORD_ADMIN_LIST_EMOJIS: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      mock_item it = mk_make_item(CCORD_NET_GUILD_EXPRESSIONS, mk_now_of(m) + m->cfg.latency_ms);
      it.emojis_own = ccord_alloc_array(m->nemojis ? m->nemojis : 1u, sizeof *it.emojis_own);
      if (!it.emojis_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t k = 0;
      for (size_t i = 0; i < m->nemojis; i++) {
        if (m->emojis[i].guild_id == g) it.emojis_own[k++] = m->emojis[i];
      }
      it.ev.guild_id = g;
      it.ev.emojis = it.emojis_own;
      it.ev.emoji_count = k;
      it.ev.replace_emojis = true;
      it.ev.expressions_admin = true;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_EMOJI: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      uint32_t slot_cnt = 0;
      size_t ins = m->nemojis;
      for (size_t i = 0; i < m->nemojis; i++) {
        if (m->emojis[i].guild_id == g) {
          if (m->emojis[i].animated == req->emoji_animated) slot_cnt++;
          ins = i + 1u;
        }
      }
      uint32_t limit = ccord_guild_emoji_slot_limit(m->guilds[gi].premium_tier);
      if (slot_cnt >= limit || m->nemojis >= MOCK_EMOJIS_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE,
                                  req->emoji_animated ? "Maximum number of animated emojis reached"
                                                      : "Maximum number of emojis reached",
                                  err);
        return true;
      }
      const char *name = mk_admin_owned(m, req->name);
      if (!name) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      ccord_id id = 70000u + (m->next_admin_id++);
      memmove(&m->emojis[ins + 1u], &m->emojis[ins], (m->nemojis - ins) * sizeof m->emojis[0]);
      m->emojis[ins] = (ccord_emoji){
        .id = id, .guild_id = g, .name = name, .animated = req->emoji_animated, .available = true,
        .user_id = m->self_id, .user_name = m->users[0].name,
      };
      m->nemojis++;
      *out = mock_audit_add(m, g, 60, id, name, req->reason, name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_EMOJI_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.emojis_own = ccord_alloc_array(1u, sizeof *it.emojis_own);
        if (!it.emojis_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.emojis_own[0] = m->emojis[ins];
        it.ev.guild_id = g;
        it.ev.emojis = it.emojis_own;
        it.ev.emoji_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_EMOJI: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ei = SIZE_MAX;
      for (size_t i = 0; i < m->nemojis; i++) {
        if (m->emojis[i].guild_id == g && m->emojis[i].id == req->expr_id) { ei = i; break; }
      }
      if (ei == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Emoji", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->emojis[ei].user_id)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *name = mk_admin_owned(m, req->name);
      if (!name) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      char det[128];
      (void)snprintf(det, sizeof det, "name: %.40s -> %.40s", m->emojis[ei].name, name);
      m->emojis[ei].name = name;
      *out = mock_audit_add(m, g, 61, req->expr_id, name, req->reason, det);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_EMOJI_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.emojis_own = ccord_alloc_array(1u, sizeof *it.emojis_own);
        if (!it.emojis_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.emojis_own[0] = m->emojis[ei];
        it.ev.guild_id = g;
        it.ev.emojis = it.emojis_own;
        it.ev.emoji_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_EMOJI: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t ei = SIZE_MAX;
      for (size_t i = 0; i < m->nemojis; i++) {
        if (m->emojis[i].guild_id == g && m->emojis[i].id == req->expr_id) { ei = i; break; }
      }
      if (ei == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Emoji", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->emojis[ei].user_id)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *old_name = m->emojis[ei].name;
      memmove(&m->emojis[ei], &m->emojis[ei + 1u], (m->nemojis - ei - 1u) * sizeof m->emojis[0]);
      m->nemojis--;
      *out = mock_audit_add(m, g, 62, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_EMOJI_DELETE, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_STICKERS: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      mock_item it = mk_make_item(CCORD_NET_GUILD_EXPRESSIONS, mk_now_of(m) + m->cfg.latency_ms);
      it.stickers_own = ccord_alloc_array(m->nstickers ? m->nstickers : 1u, sizeof *it.stickers_own);
      if (!it.stickers_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t k = 0;
      for (size_t i = 0; i < m->nstickers; i++) {
        if (m->stickers[i].guild_id == g) it.stickers_own[k++] = m->stickers[i];
      }
      it.ev.guild_id = g;
      it.ev.stickers = it.stickers_own;
      it.ev.sticker_count = k;
      it.ev.replace_stickers = true;
      it.ev.expressions_admin = true;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_STICKER: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      uint32_t slot_cnt = 0;
      size_t ins = m->nstickers;
      for (size_t i = 0; i < m->nstickers; i++) {
        if (m->stickers[i].guild_id == g) {
          slot_cnt++;
          ins = i + 1u;
        }
      }
      uint32_t limit = ccord_guild_sticker_slot_limit(m->guilds[gi].premium_tier);
      if (slot_cnt >= limit || m->nstickers >= MOCK_STICKERS_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of stickers reached", err);
        return true;
      }
      const char *name = mk_admin_owned(m, req->name);
      const char *desc = mk_admin_owned(m, req->sticker_desc);
      const char *tags = mk_admin_owned(m, req->sticker_tags);
      if (!name || !desc || !tags) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      ccord_id id = 70000u + (m->next_admin_id++);
      memmove(&m->stickers[ins + 1u], &m->stickers[ins], (m->nstickers - ins) * sizeof m->stickers[0]);
      m->stickers[ins] = (ccord_sticker){
        .id = id, .guild_id = g, .name = name, .description = desc, .tags = tags,
        .format = req->sticker_format ? req->sticker_format : CCORD_STICKER_PNG, .available = true,
        .user_id = m->self_id, .user_name = m->users[0].name,
      };
      m->nstickers++;
      *out = mock_audit_add(m, g, 90, id, name, req->reason, name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_STICKER_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.stickers_own = ccord_alloc_array(1u, sizeof *it.stickers_own);
        if (!it.stickers_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.stickers_own[0] = m->stickers[ins];
        it.ev.guild_id = g;
        it.ev.stickers = it.stickers_own;
        it.ev.sticker_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_STICKER: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t si = SIZE_MAX;
      for (size_t i = 0; i < m->nstickers; i++) {
        if (m->stickers[i].guild_id == g && m->stickers[i].id == req->expr_id) { si = i; break; }
      }
      if (si == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Sticker", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->stickers[si].user_id)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char det[128];
      det[0] = '\0';
      if (req->name[0] != '\0') {
        const char *name = mk_admin_owned(m, req->name);
        if (!name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        (void)snprintf(det, sizeof det, "name: %.40s -> %.40s", m->stickers[si].name, name);
        m->stickers[si].name = name;
      }
      if (req->has_sticker_desc) {
        const char *desc = mk_admin_owned(m, req->sticker_desc);
        if (!desc) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        if (det[0] == '\0') (void)snprintf(det, sizeof det, "description: %.40s -> %.40s", m->stickers[si].description, desc);
        m->stickers[si].description = desc;
      }
      if (req->has_sticker_tags) {
        const char *tags = mk_admin_owned(m, req->sticker_tags);
        if (!tags) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        if (det[0] == '\0') (void)snprintf(det, sizeof det, "tags: %.40s -> %.40s", m->stickers[si].tags, tags);
        m->stickers[si].tags = tags;
      }
      *out = mock_audit_add(m, g, 91, req->expr_id, m->stickers[si].name, req->reason, det);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_STICKER_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.stickers_own = ccord_alloc_array(1u, sizeof *it.stickers_own);
        if (!it.stickers_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.stickers_own[0] = m->stickers[si];
        it.ev.guild_id = g;
        it.ev.stickers = it.stickers_own;
        it.ev.sticker_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_STICKER: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t si = SIZE_MAX;
      for (size_t i = 0; i < m->nstickers; i++) {
        if (m->stickers[i].guild_id == g && m->stickers[i].id == req->expr_id) { si = i; break; }
      }
      if (si == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Sticker", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->stickers[si].user_id)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *old_name = m->stickers[si].name;
      memmove(&m->stickers[si], &m->stickers[si + 1u], (m->nstickers - si - 1u) * sizeof m->stickers[0]);
      m->nstickers--;
      *out = mock_audit_add(m, g, 92, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_STICKER_DELETE, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_LIST_SOUNDBOARD: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      mock_item it = mk_make_item(CCORD_NET_GUILD_EXPRESSIONS, mk_now_of(m) + m->cfg.latency_ms);
      it.soundboard_own = ccord_alloc_array(m->nsoundboard ? m->nsoundboard : 1u, sizeof *it.soundboard_own);
      if (!it.soundboard_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t k = 0;
      for (size_t i = 0; i < m->nsoundboard; i++) {
        if (m->soundboard[i].guild_id == g) it.soundboard_own[k++] = m->soundboard[i];
      }
      it.ev.guild_id = g;
      it.ev.soundboard = it.soundboard_own;
      it.ev.soundboard_count = k;
      it.ev.replace_soundboard = true;
      it.ev.expressions_admin = true;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_CREATE_SOUNDBOARD: {
      size_t gi = mock_guild_index(m, g);
      if (gi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      if (!ccord_perm_can_create_expression(mock_self_perms(m, g))) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      uint32_t slot_cnt = 0;
      size_t ins = m->nsoundboard;
      for (size_t i = 0; i < m->nsoundboard; i++) {
        if (m->soundboard[i].guild_id == g) {
          slot_cnt++;
          ins = i + 1u;
        }
      }
      uint32_t limit = ccord_guild_soundboard_slot_limit(m->guilds[gi].premium_tier);
      if (slot_cnt >= limit || m->nsoundboard >= MOCK_SOUNDBOARD_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of soundboard sounds reached", err);
        return true;
      }
      const char *name = mk_admin_owned(m, req->name);
      const char *em_name = req->has_sound_emoji ? mk_admin_owned(m, req->sound_emoji_name) : "";
      if (!name || !em_name) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      ccord_id id = 70000u + (m->next_admin_id++);
      memmove(&m->soundboard[ins + 1u], &m->soundboard[ins], (m->nsoundboard - ins) * sizeof m->soundboard[0]);
      m->soundboard[ins] = (ccord_soundboard_sound){
        .id = id, .guild_id = g, .name = name,
        .volume_pct = req->sound_volume_pct >= 0 ? (uint8_t)req->sound_volume_pct : 100u,
        .emoji_id = req->has_sound_emoji ? req->sound_emoji_id : CCORD_ID_NONE,
        .emoji_name = em_name,
        .user_id = m->self_id, .user_name = m->users[0].name, .available = true,
      };
      m->nsoundboard++;
      *out = mock_audit_add(m, g, 130, id, name, req->reason, name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_SOUNDBOARD_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.soundboard_own = ccord_alloc_array(1u, sizeof *it.soundboard_own);
        if (!it.soundboard_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.soundboard_own[0] = m->soundboard[ins];
        it.ev.guild_id = g;
        it.ev.soundboard = it.soundboard_own;
        it.ev.soundboard_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_EDIT_SOUNDBOARD: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t sbi = SIZE_MAX;
      for (size_t i = 0; i < m->nsoundboard; i++) {
        if (m->soundboard[i].guild_id == g && m->soundboard[i].id == req->expr_id) { sbi = i; break; }
      }
      if (sbi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Soundboard Sound", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->soundboard[sbi].user_id)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      char det[128];
      det[0] = '\0';
      if (req->name[0] != '\0') {
        const char *name = mk_admin_owned(m, req->name);
        if (!name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        (void)snprintf(det, sizeof det, "name: %.40s -> %.40s", m->soundboard[sbi].name, name);
        m->soundboard[sbi].name = name;
      }
      if (req->sound_volume_pct >= 0) {
        if (det[0] == '\0') {
          (void)snprintf(det, sizeof det, "volume: %u -> %d", (unsigned)m->soundboard[sbi].volume_pct,
                         req->sound_volume_pct);
        }
        m->soundboard[sbi].volume_pct = (uint8_t)req->sound_volume_pct;
      }
      if (req->has_sound_emoji) {
        const char *em_name = mk_admin_owned(m, req->sound_emoji_name);
        if (!em_name) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        if (det[0] == '\0') {
          (void)snprintf(det, sizeof det, "emoji_name: %.30s -> %.30s", m->soundboard[sbi].emoji_name, em_name);
        }
        m->soundboard[sbi].emoji_id = req->sound_emoji_id;
        m->soundboard[sbi].emoji_name = em_name;
      }
      *out = mock_audit_add(m, g, 131, req->expr_id, m->soundboard[sbi].name, req->reason, det);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_SOUNDBOARD_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
        it.soundboard_own = ccord_alloc_array(1u, sizeof *it.soundboard_own);
        if (!it.soundboard_own) {
          *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          return true;
        }
        it.soundboard_own[0] = m->soundboard[sbi];
        it.ev.guild_id = g;
        it.ev.soundboard = it.soundboard_own;
        it.ev.soundboard_count = 1u;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    case CCORD_ADMIN_DELETE_SOUNDBOARD: {
      if (mock_guild_index(m, g) == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
        return true;
      }
      size_t sbi = SIZE_MAX;
      for (size_t i = 0; i < m->nsoundboard; i++) {
        if (m->soundboard[i].guild_id == g && m->soundboard[i].id == req->expr_id) { sbi = i; break; }
      }
      if (sbi == SIZE_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Soundboard Sound", err);
        return true;
      }
      if (!ccord_perm_can_modify_expression(mock_self_perms(m, g), m->self_id, m->soundboard[sbi].user_id)) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      const char *old_name = m->soundboard[sbi].name;
      memmove(&m->soundboard[sbi], &m->soundboard[sbi + 1u], (m->nsoundboard - sbi - 1u) * sizeof m->soundboard[0]);
      m->nsoundboard--;
      *out = mock_audit_add(m, g, 132, req->expr_id, old_name, req->reason, old_name);
      if (*out == CCORD_OK) {
        mock_item it = mk_make_item(CCORD_NET_SOUNDBOARD_DELETE, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = g;
        it.ev.expr_id = req->expr_id;
        *out = mk_push_item(m, &it, err);
      }
      return true;
    }
    default: return false;
  }
  return false;
}

