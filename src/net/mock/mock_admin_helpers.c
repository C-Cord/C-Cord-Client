// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---- server administration: the mock plays a small Discord -----------------------------------------------------------
// Applies the change to the mock's own dataset and emits the events the live server would, so the demo client and the
// UI tests exercise the same path as a real server. refuse_writes makes every request come back as a 403.

ccord_status mk_admin_reply_failed(mock *m, const ccord_admin_request *req, ccord_status st, const char *text,
                                       ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_ACTION_FAILED, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.action = CCORD_ACTION_ADMIN;
  it.ev.admin_op = req->op;
  it.ev.guild_id = req->guild_id;
  it.ev.channel_id = req->channel_id;
  it.ev.user_id = req->user_id;
  it.ev.expr_id = req->expr_id;
  it.ev.status = st;
  it.ev.text = text;
  return mk_push_item(m, &it, err);
}

const char *mock_user_name(const mock *m, ccord_id id)
{
  for (size_t i = 0; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == id) return m->users[i].name;
  }
  return "";
}

const char *mk_admin_owned(mock *m, const char *s)
{
  char *c = ccord_strndup(s, strlen(s));
  if (c) mk_retire_content(m, c);   // lives until destroy, so queued events never point at freed text
  return c;
}

size_t mk_admin_channel_index(const mock *m, ccord_id id)
{
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == id) return i;
  }
  return SIZE_MAX;
}

long mk_admin_member_index(const mock *m, ccord_id guild_id, ccord_id user_id)
{
  for (size_t i = 0; i < m->nmembers; i++) {
    if (m->members[i].guild_id == guild_id && m->members[i].user_id == user_id) return (long)i;
  }
  return -1;
}

size_t mock_find_overwrites(const mock *m, ccord_id channel_id)
{
  for (size_t i = 0; i < m->noverwrites; i++) {
    if (m->overwrites[i].channel_id == channel_id) return i;
  }
  return SIZE_MAX;
}

size_t mock_ensure_overwrites(mock *m, ccord_id channel_id)
{
  size_t oi = mock_find_overwrites(m, channel_id);
  if (oi != SIZE_MAX) return oi;
  if (m->noverwrites >= MOCK_MAX_OVERWRITE_CHANNELS) return SIZE_MAX;
  oi = m->noverwrites++;
  memset(m->overwrite_pool[oi], 0, sizeof m->overwrite_pool[oi]);
  m->overwrites[oi] = (ccord_channel_overwrites){
    .channel_id = channel_id,
    .items = m->overwrite_pool[oi],
    .count = 0u,
  };
  return oi;
}

static void mock_remove_channel_overwrites(mock *m, ccord_id channel_id)
{
  size_t oi = mock_find_overwrites(m, channel_id);
  if (oi == SIZE_MAX) return;
  size_t tail = m->noverwrites - oi - 1u;
  if (tail > 0u) {
    memmove(&m->overwrites[oi], &m->overwrites[oi + 1u], tail * sizeof m->overwrites[0]);
    memmove(&m->overwrite_pool[oi], &m->overwrite_pool[oi + 1u], tail * sizeof m->overwrite_pool[0]);
  }
  m->noverwrites--;
  for (size_t i = oi; i < m->noverwrites; i++) {
    m->overwrites[i].items = m->overwrite_pool[i];
  }
}

size_t mock_ensure_forum_meta(mock *m, ccord_id forum_id)
{
  size_t fmi = mock_find_forum_meta(m, forum_id);
  if (fmi != SIZE_MAX) return fmi;
  if (m->nforum_metas >= MOCK_MAX_FORUM_METAS) return SIZE_MAX;
  fmi = m->nforum_metas++;
  m->forum_metas[fmi] = (ccord_forum_meta){
    .forum_id = forum_id,
    .default_sort = CCORD_FORUM_SORT_ACTIVITY,
    .tag_count = 0u,
  };
  return fmi;
}

static void mock_remove_forum_meta(mock *m, ccord_id forum_id)
{
  size_t fmi = mock_find_forum_meta(m, forum_id);
  if (fmi == SIZE_MAX) return;
  memmove(&m->forum_metas[fmi], &m->forum_metas[fmi + 1u], (m->nforum_metas - fmi - 1u) * sizeof m->forum_metas[0]);
  m->nforum_metas--;
}

static ccord_status admin_attach_channel_meta(mock *m, mock_item *it, ccord_id cid, ccord_error *err)
{
  size_t oi = mock_find_overwrites(m, cid);
  if (oi != SIZE_MAX) {
    size_t cnt = m->overwrites[oi].count;
    it->overwrites_own = ccord_alloc_array(1u, sizeof *it->overwrites_own);
    it->overwrite_items_own = cnt > 0u ? ccord_alloc_array(cnt, sizeof *it->overwrite_items_own) : nullptr;
    if (!it->overwrites_own || (cnt > 0u && !it->overwrite_items_own)) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    if (cnt > 0u) memcpy(it->overwrite_items_own, m->overwrites[oi].items, cnt * sizeof *it->overwrite_items_own);
    it->overwrites_own[0] = (ccord_channel_overwrites){
      .channel_id = cid,
      .items = it->overwrite_items_own,
      .count = cnt,
    };
    it->ev.overwrites = it->overwrites_own;
    it->ev.overwrite_count = 1u;
  }
  size_t fmi = mock_find_forum_meta(m, cid);
  if (fmi != SIZE_MAX) {
    it->forum_metas_own = ccord_alloc_array(1u, sizeof *it->forum_metas_own);
    if (!it->forum_metas_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    it->forum_metas_own[0] = m->forum_metas[fmi];
    it->ev.forum_metas = it->forum_metas_own;
    it->ev.forum_meta_count = 1u;
  }
  return CCORD_OK;
}

ccord_status mk_admin_emit_channel_op(mock *m, size_t ci, ccord_admin_op op, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_CHANNEL_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
  it.chan = ccord_alloc_array(1, sizeof *it.chan);
  if (!it.chan) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.chan = m->channels[ci];
  it.ev.channel = it.chan;
  it.ev.channel_id = it.chan->id;
  it.ev.guild_id = it.chan->guild_id;
  it.ev.admin_op = op;
  it.ev.users = m->users;
  it.ev.user_count = MOCK_NUM_USERS;
  if (admin_attach_channel_meta(m, &it, it.chan->id, err) != CCORD_OK) {
    mk_item_release(&it);
    return CCORD_ERR_NOMEM;
  }
  return mk_push_item(m, &it, err);
}

ccord_status mk_admin_emit_channel(mock *m, size_t ci, ccord_error *err)
{
  return mk_admin_emit_channel_op(m, ci, CCORD_ADMIN_NONE, err);
}

ccord_status mk_admin_emit_member(mock *m, size_t mi, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_GUILD_MEMBERS, mk_now_of(m) + m->cfg.latency_ms);
  it.member = ccord_alloc_array(1, sizeof *it.member);
  if (!it.member) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.member = m->members[mi];
  it.ev.guild_id = it.member->guild_id;
  it.ev.guild_members = it.member;
  it.ev.guild_member_count = 1;
  return mk_push_item(m, &it, err);
}

// ---- roles, bans, invites and the audit log of the simulated server ----
// The mock answers like Discord does: permission and hierarchy checks use the same engine the client does (as the mock's
// own source of truth, from its own role data), a refusal is a 403 "Missing Permissions", and every change that Discord
// records lands in the audit log.


size_t mock_role_find(const mock *m, ccord_id guild, ccord_id role)
{
  for (size_t i = 0; i < m->nroles; i++) {
    if (m->roles[i].guild_id == guild && m->roles[i].id == role) return i;
  }
  return SIZE_MAX;
}

// The guild's roles are contiguous; returns the first index and sets *count (0 when the guild has none).
size_t mock_guild_roles(const mock *m, ccord_id guild, size_t *count)
{
  size_t first = SIZE_MAX, n = 0;
  for (size_t i = 0; i < m->nroles; i++) {
    if (m->roles[i].guild_id != guild) continue;
    if (first == SIZE_MAX) first = i;
    n++;
  }
  *count = n;
  return first == SIZE_MAX ? 0 : first;
}

mock_mrow *mock_mrow_find(mock *m, ccord_id guild, ccord_id user)
{
  for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
    if (m->mrows[i].present && m->mrows[i].guild_id == guild && m->mrows[i].user_id == user) return &m->mrows[i];
  }
  return nullptr;
}

ccord_id mock_guild_owner(const mock *m, ccord_id guild)
{
  for (size_t i = 0; i < m->nguilds; i++) {
    if (m->guilds[i].id == guild) return m->guilds[i].owner_id;
  }
  return CCORD_ID_NONE;
}

uint64_t mock_self_perms(mock *m, ccord_id guild)
{
  size_t cnt = 0, first = mock_guild_roles(m, guild, &cnt);
  mock_mrow *me = mock_mrow_find(m, guild, m->self_id);
  return ccord_perm_guild(guild, mock_guild_owner(m, guild), m->self_id, cnt ? &m->roles[first] : nullptr, cnt,
                          me ? me->ids : nullptr, me ? me->count : 0);
}

bool mock_may(mock *m, ccord_id guild, uint64_t need)
{
  return (mock_self_perms(m, guild) & need) == need;
}

uint64_t mock_channel_perms(mock *m, ccord_id guild, ccord_id channel_id)
{
  uint64_t base = mock_self_perms(m, guild);
  mock_mrow *me = mock_mrow_find(m, guild, m->self_id);
  size_t oi = mock_find_overwrites(m, channel_id);
  const ccord_overwrite *ows = oi != SIZE_MAX ? m->overwrites[oi].items : nullptr;
  size_t nows = oi != SIZE_MAX ? m->overwrites[oi].count : 0u;
  return ccord_perm_channel(base, guild, m->self_id, ows, nows,
                            me ? me->ids : nullptr, me ? me->count : 0u);
}

bool mock_may_edit_role(mock *m, ccord_id guild, ccord_id role, bool assign)
{
  size_t cnt = 0, first = mock_guild_roles(m, guild, &cnt);
  size_t ri = mock_role_find(m, guild, role);
  mock_mrow *me = mock_mrow_find(m, guild, m->self_id);
  return ccord_perm_can_edit_role(guild, mock_guild_owner(m, guild), m->self_id, mock_self_perms(m, guild),
                                  me ? me->ids : nullptr, me ? me->count : 0, ri == SIZE_MAX ? nullptr : &m->roles[ri],
                                  assign, cnt ? &m->roles[first] : nullptr, cnt) == CCORD_PERM_ALLOWED;
}

// A member's displayed role is their highest one.
void mock_refresh_display(mock *m, ccord_id guild, ccord_id user)
{
  long mi = mk_admin_member_index(m, guild, user);
  mock_mrow *r = mock_mrow_find(m, guild, user);
  if (mi < 0 || !r) return;
  const ccord_role *top = nullptr;
  for (size_t i = 0; i < r->count; i++) {
    size_t ri = mock_role_find(m, guild, r->ids[i]);
    if (ri != SIZE_MAX && (!top || m->roles[ri].position > top->position)) top = &m->roles[ri];
  }
  ccord_member *mem = &m->members[mi];
  mem->role_name = top ? top->name : "Members";
  mem->role_color = top ? top->color : 0;
  mem->role_rank = !top || top->position < 1 ? 2 : (top->position >= 3 ? 0 : (uint8_t)(3 - top->position));
}

ccord_status mock_audit_add(mock *m, ccord_id guild, uint16_t action, ccord_id target, const char *target_name,
                                   const char *reason, const char *detail)
{
  if (m->naudit == MOCK_AUDIT_MAX) {
    memmove(&m->audit[0], &m->audit[1], (MOCK_AUDIT_MAX - 1u) * sizeof m->audit[0]);
    m->naudit--;
  }
  uint64_t now = mk_now_of(m);
  ccord_id id = now > MOCK_DISCORD_EPOCH_MS ? (ccord_id)((now - MOCK_DISCORD_EPOCH_MS) << 22) : 0;
  if (id <= m->last_audit_id) id = m->last_audit_id + 1;   // strictly increasing, so "newest first" is well defined
  m->last_audit_id = id;
  const char *tn = mk_admin_owned(m, target_name ? target_name : "");
  const char *rs = mk_admin_owned(m, reason ? reason : "");
  const char *dt = mk_admin_owned(m, detail ? detail : "");
  if (!tn || !rs || !dt) return CCORD_ERR_NOMEM;
  m->audit[m->naudit++] = (mock_audit){guild, {.id = id, .time_ms = (int64_t)now, .user_id = m->self_id,
                                               .user_name = m->users[0].name, .target_id = target, .target_name = tn,
                                               .action = action, .reason = rs, .detail = dt}};
  mock_audit_fill_changes(&m->audit[m->naudit - 1].e, dt);
  return CCORD_OK;
}

ccord_status mk_admin_emit_roles(mock *m, ccord_net_event_type type, const ccord_id *ids, size_t n, ccord_id guild,
                                     ccord_error *err)
{
  mock_item it = mk_make_item(type, mk_now_of(m) + m->cfg.latency_ms);
  it.roles_own = ccord_alloc_array(n ? n : 1u, sizeof *it.roles_own);
  if (!it.roles_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    size_t ri = mock_role_find(m, guild, ids[i]);
    if (ri != SIZE_MAX) it.roles_own[k++] = m->roles[ri];
  }
  it.ev.roles = it.roles_own;
  it.ev.role_count = k;
  it.ev.guild_id = guild;
  it.ev.role_id = k ? it.roles_own[0].id : CCORD_ID_NONE;
  return mk_push_item(m, &it, err);
}

ccord_status mk_admin_emit_member_roles(mock *m, ccord_id guild, ccord_id user, ccord_id role, bool added,
                                            ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_MEMBER_ROLE_CHANGE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = guild;
  it.ev.user_id = user;
  it.ev.role_id = role;
  it.ev.role_added = added;
  ccord_status st = mk_push_item(m, &it, err);
  if (st != CCORD_OK) return st;
  long mi = mk_admin_member_index(m, guild, user);
  if (mi < 0) return CCORD_OK;
  // What the gateway's GUILD_MEMBER_UPDATE carries: the member (with the role it now shows) and the full role list.
  mock_item up = mk_make_item(CCORD_NET_GUILD_MEMBERS, mk_now_of(m) + m->cfg.latency_ms);
  up.member = ccord_alloc_array(1, sizeof *up.member);
  if (!up.member) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *up.member = m->members[mi];
  up.ev.guild_id = guild;
  up.ev.guild_members = up.member;
  up.ev.guild_member_count = 1;
  mock_sync_member_roles(m);
  for (size_t i = 0; i < m->nmember_roles; i++) {   // the array never moves, so the pointer stays valid
    if (m->member_roles[i].guild_id == guild && m->member_roles[i].user_id == user) {
      up.ev.member_roles = &m->member_roles[i];
      up.ev.member_role_count = 1;
      break;
    }
  }
  return mk_push_item(m, &up, err);
}

ccord_status mk_admin_emit_bans(mock *m, ccord_id guild, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_BAN_LIST, mk_now_of(m) + m->cfg.latency_ms);
  it.bans_own = ccord_alloc_array(m->nbans ? m->nbans : 1u, sizeof *it.bans_own);
  if (!it.bans_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = 0; i < m->nbans; i++) {
    if (m->bans[i].guild_id == guild) it.bans_own[k++] = m->bans[i].ban;
  }
  it.ev.guild_id = guild;
  it.ev.bans = it.bans_own;
  it.ev.ban_count = k;
  return mk_push_item(m, &it, err);
}

ccord_status mk_admin_emit_invites(mock *m, ccord_net_event_type type, ccord_id guild, const char *code,
                                       ccord_error *err)
{
  mock_item it = mk_make_item(type, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = guild;
  if (type == CCORD_NET_INVITE_DELETE) {
    it.ev.invite_code = code;
    return mk_push_item(m, &it, err);
  }
  it.invites_own = ccord_alloc_array(m->ninvites ? m->ninvites : 1u, sizeof *it.invites_own);
  if (!it.invites_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = 0; i < m->ninvites; i++) {
    if (m->invites[i].guild_id != guild) continue;
    if (type == CCORD_NET_INVITE_UPSERT && (!code || strcmp(m->invites[i].inv.code, code) != 0)) continue;
    it.invites_own[k++] = m->invites[i].inv;
  }
  it.ev.invites = it.invites_own;
  it.ev.invite_count = k;
  return mk_push_item(m, &it, err);
}

// Newest first, at most CCORD_ADMIN_AUDIT_LIMIT, optionally only one actor and / or one action type.
ccord_status mk_admin_emit_audit(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_AUDIT_LOG, mk_now_of(m) + m->cfg.latency_ms);
  it.audit_own = ccord_alloc_array(m->naudit ? m->naudit : 1u, sizeof *it.audit_own);
  if (!it.audit_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = m->naudit; i > 0 && k < CCORD_ADMIN_AUDIT_LIMIT; i--) {
    const mock_audit *a = &m->audit[i - 1];
    if (a->guild_id != req->guild_id) continue;
    if (req->user_id != CCORD_ID_NONE && a->e.user_id != req->user_id) continue;
    if (req->audit_action != 0 && a->e.action != req->audit_action) continue;
    it.audit_own[k++] = a->e;
  }
  it.ev.guild_id = req->guild_id;
  it.ev.audit = it.audit_own;
  it.ev.audit_count = k;
  return mk_push_item(m, &it, err);
}

// Appends a channel (and its empty message list) and returns its index, or SIZE_MAX.
size_t mk_admin_add_channel(mock *m, const ccord_channel *ch, ccord_error *err)
{
  size_t n = m->nchannels + 1;
  ccord_channel *nc = ccord_realloc_array(m->channels, n, sizeof *nc);
  if (nc) m->channels = nc;
  ccord_message **nmsgs = ccord_realloc_array(m->chmsgs, n, sizeof *nmsgs);
  if (nmsgs) m->chmsgs = nmsgs;
  size_t *nchn = ccord_realloc_array(m->chn, n, sizeof *nchn);
  if (nchn) m->chn = nchn;
  size_t *nchcap = ccord_realloc_array(m->chcap, n, sizeof *nchcap);
  if (nchcap) m->chcap = nchcap;
  if (!nc || !nmsgs || !nchn || !nchcap) {
    (void)ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    return SIZE_MAX;
  }
  size_t ci = m->nchannels;
  m->channels[ci] = *ch;
  m->chmsgs[ci] = nullptr;
  m->chn[ci] = 0;
  m->chcap[ci] = 0;
  m->nchannels = n;
  m->snap.channels = m->channels;
  if (m->snap.channel_count < m->nchannels) m->snap.channel_count = m->nchannels;
  return ci;
}

void mk_admin_remove_channel(mock *m, size_t ci)
{
  ccord_id cid = m->channels[ci].id;
  // Retire (not free) the messages' text: an already-queued history event may still point at it.
  for (size_t k = 0; m->chmsgs[ci] && k < m->chn[ci]; k++) {
    mk_retire_content(m, m->chmsgs[ci][k].content);
    mk_retire_rich(m, m->chmsgs[ci][k].rich);
    m->chmsgs[ci][k].content = nullptr;
    m->chmsgs[ci][k].rich = nullptr;
  }
  ccord_free(m->chmsgs[ci]);
  mock_remove_channel_overwrites(m, cid);
  mock_remove_forum_meta(m, cid);
  size_t inv_out = 0u;
  for (size_t i = 0u; i < m->ninvites; i++) {
    if (m->invites[i].inv.channel_id != cid) m->invites[inv_out++] = m->invites[i];
  }
  m->ninvites = inv_out;
  size_t v_out = 0u;
  for (size_t i = 0u; i < m->nvoices; i++) {
    if (m->voices[i].channel_id != cid) m->voices[v_out++] = m->voices[i];
  }
  m->nvoices = v_out;
  m->snap.voice_state_count = m->nvoices;
  size_t stg_out = 0u;
  for (size_t i = 0u; i < m->nstages; i++) {
    if (m->stages[i].channel_id != cid) m->stages[stg_out++] = m->stages[i];
  }
  m->nstages = stg_out;
  if (m->has_stream && m->stream.channel_id == cid) m->has_stream = false;
  size_t tail = m->nchannels - ci - 1;
  memmove(&m->channels[ci], &m->channels[ci + 1], tail * sizeof *m->channels);
  memmove(&m->chmsgs[ci], &m->chmsgs[ci + 1], tail * sizeof *m->chmsgs);
  memmove(&m->chn[ci], &m->chn[ci + 1], tail * sizeof *m->chn);
  memmove(&m->chcap[ci], &m->chcap[ci + 1], tail * sizeof *m->chcap);
  m->nchannels--;
  if (m->snap.channel_count > m->nchannels) m->snap.channel_count = m->nchannels;
}

void mock_remove_guild(mock *m, size_t gi)
{
  ccord_id gid = m->guilds[gi].id;
  for (size_t ci = m->nchannels; ci > 0; ci--) {
    if (m->channels[ci - 1].guild_id == gid) mk_admin_remove_channel(m, ci - 1);
  }
  size_t m_out = 0;
  for (size_t i = 0; i < m->nmembers; i++) {
    if (m->members[i].guild_id != gid) m->members[m_out++] = m->members[i];
  }
  m->nmembers = m_out;
  m->snap.member_count = m->nmembers;
  for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
    if (m->mrows[i].guild_id == gid) m->mrows[i].present = false;
  }
  mock_sync_member_roles(m);
  size_t r_out = 0;
  for (size_t i = 0; i < m->nroles; i++) {
    if (m->roles[i].guild_id != gid) m->roles[r_out++] = m->roles[i];
  }
  m->nroles = r_out;
  size_t v_out = 0;
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].guild_id != gid) m->voices[v_out++] = m->voices[i];
  }
  m->nvoices = v_out;
  size_t e_out = 0;
  for (size_t i = 0; i < m->nemojis; i++) {
    if (m->emojis[i].guild_id != gid) m->emojis[e_out++] = m->emojis[i];
  }
  m->nemojis = e_out;
  size_t st_out = 0;
  for (size_t i = 0; i < m->nstickers; i++) {
    if (m->stickers[i].guild_id != gid) m->stickers[st_out++] = m->stickers[i];
  }
  m->nstickers = st_out;
  size_t sb_out = 0;
  for (size_t i = 0; i < m->nsoundboard; i++) {
    if (m->soundboard[i].guild_id != gid) m->soundboard[sb_out++] = m->soundboard[i];
  }
  m->nsoundboard = sb_out;
  size_t ev_out = 0;
  for (size_t i = 0; i < m->nevents; i++) {
    if (m->events[i].guild_id != gid) m->events[ev_out++] = m->events[i];
  }
  m->nevents = ev_out;
  if (m->has_stream && m->stream.guild_id == gid) m->has_stream = false;
  memmove(&m->guilds[gi], &m->guilds[gi + 1], (m->nguilds - gi - 1) * sizeof m->guilds[0]);
  m->nguilds--;
  m->snap.guild_count = m->nguilds;
}

size_t mock_guild_index(const mock *m, ccord_id gid)
{
  for (size_t i = 0; i < m->nguilds; i++) {
    if (m->guilds[i].id == gid) return i;
  }
  return SIZE_MAX;
}

ccord_status mock_emit_guild_upsert(mock *m, size_t gi, ccord_admin_op op, ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_GUILD_UPSERT, mk_now_of(m) + m->cfg.latency_ms);
  it.guild = ccord_alloc_array(1, sizeof *it.guild);
  if (!it.guild) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.guild = m->guilds[gi];
  it.ev.guild = it.guild;
  it.ev.guild_id = it.guild->id;
  it.ev.admin_op = op;
  return mk_push_item(m, &it, err);
}

// Handles the roles / bans / invites / audit-log / server-settings operations. Returns false when `req` is not one of
// them; otherwise *out is the status to hand back to the caller (failures are reported through an ACTION_FAILED event, like Discord).
static const char *mock_jstr(const ccord_json *obj, const char *key, size_t *len)
{
  if (!obj) return nullptr;
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_string(v, len) : nullptr;
}

static bool mock_ji64(const ccord_json *obj, const char *key, int64_t *out)
{
  if (!obj) return false;
  const ccord_json *v = ccord_json_get(obj, key);
  return v ? ccord_json_int64(v, out) : false;
}

static const char **mock_arena_dup_strs(ccord_arena *a, const ccord_json *arr, size_t max_n, size_t *out_n)
{
  *out_n = 0u;
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return nullptr;
  size_t n = ccord_json_array_len(arr);
  if (n > max_n) n = max_n;
  if (n == 0u) return nullptr;
  const char **list = ccord_arena_alloc(a, n * sizeof *list, alignof(const char *));
  if (!list) return nullptr;
  size_t k = 0u;
  for (size_t i = 0u; i < n; i++) {
    const ccord_json *el = ccord_json_array_at(arr, i);
    size_t slen = 0u;
    const char *s = el ? ccord_json_string(el, &slen) : nullptr;
    if (!s) continue;
    char *dup = ccord_arena_strndup(a, s, slen);
    if (!dup) return nullptr;
    list[k++] = dup;
  }
  *out_n = k;
  return k ? list : nullptr;
}

static size_t mock_json_id_array(const ccord_json *arr, ccord_id *out, size_t cap)
{
  if (!arr || ccord_json_type_of(arr) != CCORD_JSON_ARRAY) return 0u;
  size_t n = ccord_json_array_len(arr), k = 0u;
  for (size_t i = 0u; i < n && k < cap; i++) {
    const ccord_json *el = ccord_json_array_at(arr, i);
    size_t slen = 0u;
    const char *s = el ? ccord_json_string(el, &slen) : nullptr;
    ccord_id id = CCORD_ID_NONE;
    if (s && ccord_id_parse(s, slen, &id)) out[k++] = id;
  }
  return k;
}

bool mock_parse_automod_body(mock *m, const char *body, bool is_create, ccord_automod_rule *io)
{
  ccord_arena tmp;
  ccord_arena_init(&tmp, 4096u);
  ccord_error jerr;
  const ccord_json *root = ccord_json_parse(&tmp, body, strlen(body), nullptr, &jerr);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&tmp);
    return false;
  }
  size_t nlen = 0u;
  const char *nm = mock_jstr(root, "name", &nlen);
  if (nm) {
    char *dup = ccord_arena_strndup(&m->automod_arena, nm, nlen);
    if (!dup) {
      ccord_arena_free(&tmp);
      return false;
    }
    io->name = dup;
  }
  int64_t iv = 0;
  if (mock_ji64(root, "event_type", &iv) && iv > 0 && iv <= 255) {
    io->event_type = (uint8_t)iv;
  } else if (is_create) {
    io->event_type = CCORD_AUTOMOD_EVENT_MESSAGE_SEND;
  }
  if (is_create && mock_ji64(root, "trigger_type", &iv) && iv > 0 && iv <= 255) {
    io->trigger_type = (uint8_t)iv;
  }
  const ccord_json *en = ccord_json_get(root, "enabled");
  if (en && ccord_json_type_of(en) == CCORD_JSON_BOOL) io->enabled = ccord_json_bool(en, true);

  const ccord_json *meta = ccord_json_get(root, "trigger_metadata");
  if (meta && ccord_json_type_of(meta) == CCORD_JSON_OBJECT) {
    io->keywords = mock_arena_dup_strs(&m->automod_arena, ccord_json_get(meta, "keyword_filter"),
                                       CCORD_AUTOMOD_MAX_KEYWORDS, &io->keyword_count);
    io->regex_patterns = mock_arena_dup_strs(&m->automod_arena, ccord_json_get(meta, "regex_patterns"),
                                             CCORD_AUTOMOD_MAX_REGEX, &io->regex_count);
    io->allow_list = mock_arena_dup_strs(&m->automod_arena, ccord_json_get(meta, "allow_list"),
                                         CCORD_AUTOMOD_MAX_ALLOW_LIST, &io->allow_count);
    const ccord_json *parr = ccord_json_get(meta, "presets");
    if (parr && ccord_json_type_of(parr) == CCORD_JSON_ARRAY) {
      uint32_t mask = 0u;
      for (size_t i = 0u; i < ccord_json_array_len(parr); i++) {
        const ccord_json *pe = ccord_json_array_at(parr, i);
        int64_t pv = 0;
        if (pe && ccord_json_int64(pe, &pv) && pv >= 1 && pv <= 3) {
          mask |= (1u << (uint32_t)(pv - 1));
        }
      }
      io->presets = mask;
    }
    if (mock_ji64(meta, "mention_total_limit", &iv) && iv >= 0 && iv <= 255) {
      io->mention_total_limit = (uint8_t)iv;
    }
    const ccord_json *mrp = ccord_json_get(meta, "mention_raid_protection_enabled");
    if (mrp && ccord_json_type_of(mrp) == CCORD_JSON_BOOL) {
      io->mention_raid_protection = ccord_json_bool(mrp, false);
    }
  }

  const ccord_json *acts = ccord_json_get(root, "actions");
  if (acts && ccord_json_type_of(acts) == CCORD_JSON_ARRAY) {
    io->action_block = false;
    io->custom_message = "";
    io->action_alert = false;
    io->alert_channel_id = CCORD_ID_NONE;
    io->action_timeout = false;
    io->timeout_seconds = 0u;
    io->action_block_interaction = false;
    for (size_t i = 0u; i < ccord_json_array_len(acts); i++) {
      const ccord_json *a = ccord_json_array_at(acts, i);
      if (!a || ccord_json_type_of(a) != CCORD_JSON_OBJECT) continue;
      int64_t at = 0;
      if (!mock_ji64(a, "type", &at)) continue;
      const ccord_json *am = ccord_json_get(a, "metadata");
      if (am && ccord_json_type_of(am) != CCORD_JSON_OBJECT) am = nullptr;
      if (at == CCORD_AUTOMOD_ACTION_BLOCK_MESSAGE) {
        io->action_block = true;
        size_t clen = 0u;
        const char *cm = mock_jstr(am, "custom_message", &clen);
        if (cm && clen > 0u) {
          char *dup = ccord_arena_strndup(&m->automod_arena, cm, clen);
          if (dup) io->custom_message = dup;
        }
      } else if (at == CCORD_AUTOMOD_ACTION_SEND_ALERT_MESSAGE) {
        size_t clen = 0u;
        const char *cs = mock_jstr(am, "channel_id", &clen);
        ccord_id cid = CCORD_ID_NONE;
        if (cs && ccord_id_parse(cs, clen, &cid)) {
          io->action_alert = true;
          io->alert_channel_id = cid;
        }
      } else if (at == CCORD_AUTOMOD_ACTION_TIMEOUT) {
        int64_t dur = 0;
        if (mock_ji64(am, "duration_seconds", &dur) && dur > 0) {
          io->action_timeout = true;
          io->timeout_seconds = (uint32_t)dur;
        }
      } else if (at == CCORD_AUTOMOD_ACTION_BLOCK_MEMBER_INTERACTION) {
        io->action_block_interaction = true;
      }
    }
  }

  const ccord_json *er = ccord_json_get(root, "exempt_roles");
  if (er && ccord_json_type_of(er) == CCORD_JSON_ARRAY) {
    io->exempt_role_count = mock_json_id_array(er, io->exempt_role_ids, CCORD_AUTOMOD_MAX_EXEMPT_ROLES);
  }
  const ccord_json *ec = ccord_json_get(root, "exempt_channels");
  if (ec && ccord_json_type_of(ec) == CCORD_JSON_ARRAY) {
    io->exempt_channel_count = mock_json_id_array(ec, io->exempt_channel_ids, CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS);
  }
  ccord_arena_free(&tmp);
  return true;
}

const char *mock_arena_dup_cstr(ccord_arena *a, const char *s, size_t len)
{
  if (!s) return "";
  char *d = ccord_arena_strndup(a, s, len);
  return d ? d : "";
}

bool mock_parse_welcome_body(mock *m, ccord_id g, const char *body, ccord_welcome_screen *out_ws,
                                    ccord_welcome_channel out_chs[CCORD_WELCOME_MAX_CHANNELS])
{
  ccord_arena tmp;
  ccord_arena_init(&tmp, 4096u);
  ccord_error jerr;
  const ccord_json *root = ccord_json_parse(&tmp, body, strlen(body), nullptr, &jerr);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&tmp);
    return false;
  }
  *out_ws = (ccord_welcome_screen){.description = ""};
  const ccord_json *enj = ccord_json_get(root, "enabled");
  out_ws->enabled = (enj && ccord_json_type_of(enj) == CCORD_JSON_BOOL) ? ccord_json_bool(enj, false) : true;
  size_t dlen = 0u;
  const char *desc = mock_jstr(root, "description", &dlen);
  out_ws->description = mock_arena_dup_cstr(&m->automod_arena, desc, dlen);
  const ccord_json *carr = ccord_json_get(root, "welcome_channels");
  if (carr && ccord_json_type_of(carr) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(carr);
    if (n > CCORD_WELCOME_MAX_CHANNELS) n = CCORD_WELCOME_MAX_CHANNELS;
    size_t k = 0u;
    for (size_t i = 0u; i < n; i++) {
      const ccord_json *cj = ccord_json_array_at(carr, i);
      if (!cj || ccord_json_type_of(cj) != CCORD_JSON_OBJECT) continue;
      size_t cslen = 0u;
      const char *cs = mock_jstr(cj, "channel_id", &cslen);
      ccord_id cid = CCORD_ID_NONE;
      if (!cs || !ccord_id_parse(cs, cslen, &cid) || cid == CCORD_ID_NONE) {
        ccord_arena_free(&tmp);
        return false;
      }
      size_t chi = mk_admin_channel_index(m, cid);
      if (chi == SIZE_MAX || m->channels[chi].guild_id != g || m->channels[chi].kind == CCORD_CHANNEL_CATEGORY) {
        ccord_arena_free(&tmp);
        return false;
      }
      size_t cdlen = 0u, emlen = 0u, eidlen = 0u;
      const char *cd = mock_jstr(cj, "description", &cdlen);
      const char *em = mock_jstr(cj, "emoji_name", &emlen);
      const char *eids = mock_jstr(cj, "emoji_id", &eidlen);
      ccord_id eid = CCORD_ID_NONE;
      if (eids) (void)ccord_id_parse(eids, eidlen, &eid);
      out_chs[k++] = (ccord_welcome_channel){
        .channel_id = cid,
        .description = mock_arena_dup_cstr(&m->automod_arena, cd, cdlen),
        .emoji_id = eid,
        .emoji_name = mock_arena_dup_cstr(&m->automod_arena, em, emlen),
      };
    }
    out_ws->channels = k > 0u ? out_chs : nullptr;
    out_ws->channel_count = k;
  }
  ccord_arena_free(&tmp);
  return true;
}

bool mock_parse_onboarding_body(mock *m, ccord_id g, const char *body, ccord_onboarding *out_ob)
{
  ccord_arena tmp;
  ccord_arena_init(&tmp, 4096u);
  ccord_error jerr;
  const ccord_json *root = ccord_json_parse(&tmp, body, strlen(body), nullptr, &jerr);
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    ccord_arena_free(&tmp);
    return false;
  }
  *out_ob = (ccord_onboarding){.guild_id = g};
  const ccord_json *enj = ccord_json_get(root, "enabled");
  out_ob->enabled = (enj && ccord_json_type_of(enj) == CCORD_JSON_BOOL) ? ccord_json_bool(enj, false) : false;
  int64_t mode = 0;
  if (mock_ji64(root, "mode", &mode) && mode == 1) out_ob->mode = CCORD_ONBOARDING_ADVANCED;
  const ccord_json *dc = ccord_json_get(root, "default_channel_ids");
  if (dc && ccord_json_type_of(dc) == CCORD_JSON_ARRAY) {
    out_ob->default_channel_count = mock_json_id_array(dc, out_ob->default_channel_ids, CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS);
    for (size_t i = 0u; i < out_ob->default_channel_count; i++) {
      size_t chi = mk_admin_channel_index(m, out_ob->default_channel_ids[i]);
      if (chi == SIZE_MAX || m->channels[chi].guild_id != g || m->channels[chi].kind == CCORD_CHANNEL_CATEGORY) {
        ccord_arena_free(&tmp);
        return false;
      }
    }
  }
  const ccord_json *parr = ccord_json_get(root, "prompts");
  if (parr && ccord_json_type_of(parr) == CCORD_JSON_ARRAY) {
    size_t pn = ccord_json_array_len(parr);
    if (pn > CCORD_ONBOARDING_MAX_PROMPTS) pn = CCORD_ONBOARDING_MAX_PROMPTS;
    ccord_onboarding_prompt *prompts =
      pn > 0u ? ccord_arena_alloc(&m->automod_arena, pn * sizeof *prompts, alignof(ccord_onboarding_prompt)) : nullptr;
    size_t pk = 0u;
    for (size_t i = 0u; prompts && i < pn; i++) {
      const ccord_json *pj = ccord_json_array_at(parr, i);
      if (!pj || ccord_json_type_of(pj) != CCORD_JSON_OBJECT) continue;
      ccord_onboarding_prompt pr = {0};
      size_t pidlen = 0u, ptlen = 0u;
      const char *pids = mock_jstr(pj, "id", &pidlen);
      ccord_id pid = CCORD_ID_NONE;
      if (pids && ccord_id_parse(pids, pidlen, &pid) && pid >= 1000u) pr.id = pid;
      else pr.id = m->next_onboarding_id++;
      int64_t ptype = 0;
      if (mock_ji64(pj, "type", &ptype) && ptype == 1) pr.type = CCORD_ONBOARDING_DROPDOWN;
      const char *pt = mock_jstr(pj, "title", &ptlen);
      if (!pt || ptlen == 0u) continue;
      pr.title = mock_arena_dup_cstr(&m->automod_arena, pt, ptlen);
      const ccord_json *ssj = ccord_json_get(pj, "single_select");
      const ccord_json *rqj = ccord_json_get(pj, "required");
      const ccord_json *ioj = ccord_json_get(pj, "in_onboarding");
      pr.single_select = ssj && ccord_json_bool(ssj, false);
      pr.required = rqj && ccord_json_bool(rqj, false);
      pr.in_onboarding = !ioj || ccord_json_bool(ioj, true);
      const ccord_json *oarr = ccord_json_get(pj, "options");
      if (oarr && ccord_json_type_of(oarr) == CCORD_JSON_ARRAY) {
        size_t on = ccord_json_array_len(oarr);
        if (on > CCORD_ONBOARDING_MAX_OPTIONS) on = CCORD_ONBOARDING_MAX_OPTIONS;
        ccord_onboarding_option *opts =
          on > 0u ? ccord_arena_alloc(&m->automod_arena, on * sizeof *opts, alignof(ccord_onboarding_option)) : nullptr;
        size_t ok = 0u;
        for (size_t j = 0u; opts && j < on; j++) {
          const ccord_json *oj = ccord_json_array_at(oarr, j);
          if (!oj || ccord_json_type_of(oj) != CCORD_JSON_OBJECT) continue;
          ccord_onboarding_option opt = {0};
          size_t oidlen = 0u, otlen = 0u, odlen = 0u, oelen = 0u, oeidlen = 0u;
          const char *oids = mock_jstr(oj, "id", &oidlen);
          ccord_id oid = CCORD_ID_NONE;
          if (oids && ccord_id_parse(oids, oidlen, &oid) && oid >= 1000u) opt.id = oid;
          else opt.id = m->next_onboarding_id++;
          const char *ot = mock_jstr(oj, "title", &otlen);
          if (!ot || otlen == 0u) continue;
          opt.title = mock_arena_dup_cstr(&m->automod_arena, ot, otlen);
          const char *od = mock_jstr(oj, "description", &odlen);
          opt.description = mock_arena_dup_cstr(&m->automod_arena, od, odlen);
          const char *oe = mock_jstr(oj, "emoji_name", &oelen);
          opt.emoji_name = mock_arena_dup_cstr(&m->automod_arena, oe, oelen);
          const char *oeids = mock_jstr(oj, "emoji_id", &oeidlen);
          if (oeids) (void)ccord_id_parse(oeids, oeidlen, &opt.emoji_id);
          opt.channel_count = mock_json_id_array(ccord_json_get(oj, "channel_ids"), opt.channel_ids,
                                                 CCORD_ONBOARDING_MAX_OPT_CHANNELS);
          opt.role_count = mock_json_id_array(ccord_json_get(oj, "role_ids"), opt.role_ids,
                                              CCORD_ONBOARDING_MAX_OPT_ROLES);
          for (size_t k = 0u; k < opt.channel_count; k++) {
            size_t chi = mk_admin_channel_index(m, opt.channel_ids[k]);
            if (chi == SIZE_MAX || m->channels[chi].guild_id != g) {
              ccord_arena_free(&tmp);
              return false;
            }
          }
          opts[ok++] = opt;
        }
        pr.options = ok > 0u ? opts : nullptr;
        pr.option_count = ok;
      }
      if (pr.option_count > 0u) prompts[pk++] = pr;
    }
    out_ob->prompts = pk > 0u ? prompts : nullptr;
    out_ob->prompt_count = pk;
  }
  ccord_arena_free(&tmp);
  return true;
}

bool mock_member_prunable(mock *m, ccord_id g, size_t mi, uint8_t days, const ccord_id *inc_roles, size_t ninc)
{
  const ccord_member *mem = &m->members[mi];
  if (mem->guild_id != g || mem->user_id == m->self_id || mem->user_id == mock_guild_owner(m, g)) return false;
  const ccord_user *u = nullptr;
  for (size_t i = 0u; i < MOCK_NUM_USERS; i++) {
    if (m->users[i].id == mem->user_id) { u = &m->users[i]; break; }
  }
  if (!u || u->is_bot || u->presence != CCORD_PRESENCE_OFFLINE) return false;
  int64_t ref_now = (int64_t)mk_now_of(m) >= (int64_t)MOCK_DISCORD_EPOCH_MS ? (int64_t)mk_now_of(m) : m->cfg.base_time_ms;
  if (mem->joined_at_ms > 0 && (ref_now - mem->joined_at_ms) < (int64_t)days * 86400000ll) return false;
  mock_mrow *row = mock_mrow_find(m, g, mem->user_id);
  if (!row || !row->present) return false;
  for (size_t i = 0u; i < row->count; i++) {
    bool allowed = false;
    for (size_t j = 0u; j < ninc; j++) {
      if (inc_roles[j] == row->ids[i]) { allowed = true; break; }
    }
    if (!allowed) return false;
  }
  return true;
}

// ---- M5 admin ops: voice-channel status & stage moderation ----------

bool mk_admin_m5_voice_status(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  size_t ci = mk_admin_channel_index(m, req->channel_id);
  if (ci == SIZE_MAX) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "It no longer exists.", err);
    return true;
  }
  ccord_id g = m->channels[ci].guild_id;
  if (g == CCORD_DM_GUILD_ID ||
      (m->channels[ci].kind != CCORD_CHANNEL_VOICE && m->channels[ci].kind != CCORD_CHANNEL_STAGE)) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: not a voice channel", err);
    return true;
  }
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_SET_VOICE_CHANNEL_STATUS) == 0u) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  *out = mk_queue_voice_status(m, g, req->channel_id, req->voice_status, mk_now_of(m) + m->cfg.latency_ms, err);
  return true;
}

bool mk_admin_m5_stage_request_speak(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  ccord_id g = req->guild_id;
  if (mock_guild_index(m, g) == SIZE_MAX) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
    return true;
  }
  size_t ci = mk_admin_channel_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].guild_id != g || m->channels[ci].kind != CCORD_CHANNEL_STAGE) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: not a stage channel of the guild", err);
    return true;
  }
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_REQUEST_TO_SPEAK) == 0u) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  long vi = mock_voice_index(m, g, m->self_id);
  if (vi < 0 || m->voices[vi].channel_id != req->channel_id) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: join the stage before asking to speak", err);
    return true;
  }
  if (req->request_to_speak_ms != 0 && !m->voices[vi].suppress) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: already a stage speaker", err);
    return true;
  }
  m->voices[vi].request_to_speak_timestamp = req->request_to_speak_ms;
  *out = mk_queue_voice_state(m, &m->voices[vi], mk_now_of(m) + m->cfg.latency_ms, err);
  return true;
}

bool mk_admin_m5_stage_set_speaker(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  ccord_id g = req->guild_id;
  if (mock_guild_index(m, g) == SIZE_MAX) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
    return true;
  }
  size_t ci = mk_admin_channel_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].guild_id != g || m->channels[ci].kind != CCORD_CHANNEL_STAGE) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "mock: not a stage channel of the guild", err);
    return true;
  }
  // Discord answers 403 before it looks at the target's state.
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MUTE_MEMBERS) == 0u) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  long mi = mk_admin_member_index(m, g, req->user_id);
  if (mi < 0) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Member", err);
    return true;
  }
  long vi = mock_voice_index(m, g, req->user_id);
  if (vi < 0 || m->voices[vi].channel_id != req->channel_id) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Target user is not connected to voice.", err);
    return true;
  }
  char det[48];
  (void)snprintf(det, sizeof det, "suppress: %s -> %s", m->voices[vi].suppress ? "true" : "false",
                 req->stage_speaker ? "false" : "true");
  m->voices[vi].suppress = !req->stage_speaker;
  if (req->stage_speaker) m->voices[vi].request_to_speak_timestamp = 0;   // the invite answers the ask
  *out = mock_audit_add(m, g, 24, req->user_id, mock_user_name(m, req->user_id), req->reason, det);
  if (*out == CCORD_OK) *out = mk_queue_voice_state(m, &m->voices[vi], mk_now_of(m) + m->cfg.latency_ms, err);
  return true;
}

bool mk_admin_m5_stage_topic(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  size_t ci = mk_admin_channel_index(m, req->channel_id);
  long si = mock_stage_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].kind != CCORD_CHANNEL_STAGE || si < 0) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "It no longer exists.", err);
    return true;
  }
  ccord_id g = m->channels[ci].guild_id;
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MUTE_MEMBERS) == 0u) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  const char *topic = req->has_topic ? req->topic : "";
  // The model's inline topic is byte-sized: refuse what cannot be stored whole rather than cut a
  // multibyte character in half (the builder already enforced the 120-character limit).
  if (strlen(topic) >= sizeof m->stages[si].topic) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "topic is too long", err);
    return true;
  }
  char old_topic[sizeof m->stages[si].topic];
  memcpy(old_topic, m->stages[si].topic, sizeof old_topic);
  size_t tn = strlen(topic);   // the bounds check above guarantees tn + 1 fits
  memcpy(m->stages[si].topic, topic, tn + 1u);
  char det[288];
  (void)snprintf(det, sizeof det, "topic: %s -> %s", old_topic, m->stages[si].topic);
  *out = mock_audit_add(m, g, 84, req->channel_id, m->channels[ci].name, req->reason, det);
  if (*out == CCORD_OK) *out = mk_queue_stage_instance(m, &m->stages[si], false, mk_now_of(m) + m->cfg.latency_ms, err);
  return true;
}

bool mk_admin_m5_stage_create(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  size_t ci = mk_admin_channel_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].kind != CCORD_CHANNEL_STAGE ||
      (req->guild_id != CCORD_ID_NONE && m->channels[ci].guild_id != req->guild_id)) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "It no longer exists.", err);
    return true;
  }
  ccord_id g = m->channels[ci].guild_id;
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MUTE_MEMBERS) == 0u) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  if (mock_stage_index(m, req->channel_id) >= 0) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "A Stage Instance already exists for this channel.", err);
    return true;
  }
  const char *topic = req->has_topic ? req->topic : "";
  if (strlen(topic) >= sizeof m->stages[0].topic) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "topic is too long", err);
    return true;
  }
  if (m->nstages >= MOCK_STAGES_MAX) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "mock: too many stage instances", err);
    return true;
  }
  if (m->next_stage_id == 0) m->next_stage_id = 96001u;
  ccord_stage_instance stage = {
    .id = m->next_stage_id++, .channel_id = req->channel_id, .guild_id = g, .privacy_level = 2,
  };
  size_t tn = strlen(topic);
  memcpy(stage.topic, topic, tn + 1u);
  m->stages[m->nstages++] = stage;
  *out = mock_audit_add(m, g, 83, req->channel_id, m->channels[ci].name, req->reason, stage.topic);
  if (*out == CCORD_OK) *out = mk_queue_stage_instance(m, &m->stages[m->nstages - 1u], false, mk_now_of(m) + m->cfg.latency_ms, err);
  return true;
}

bool mk_admin_m5_stage_delete(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  size_t ci = mk_admin_channel_index(m, req->channel_id);
  long si = mock_stage_index(m, req->channel_id);
  if (ci == SIZE_MAX || m->channels[ci].kind != CCORD_CHANNEL_STAGE || si < 0) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "It no longer exists.", err);
    return true;
  }
  ccord_id g = m->channels[ci].guild_id;
  if ((mock_channel_perms(m, g, req->channel_id) & CCORD_PERM_MUTE_MEMBERS) == 0u) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  ccord_stage_instance ended = m->stages[si];
  memmove(&m->stages[si], &m->stages[si + 1], (m->nstages - (size_t)si - 1u) * sizeof m->stages[0]);
  m->nstages--;
  *out = mock_audit_add(m, g, 85, req->channel_id, m->channels[ci].name, req->reason, ended.topic);
  if (*out == CCORD_OK) *out = mk_queue_stage_instance(m, &ended, true, mk_now_of(m) + m->cfg.latency_ms, err);
  return true;
}

// ---- Guild scheduled events: /guilds/{g}/scheduled-events (LIST / CREATE / MODIFY / DELETE / RSVP) -

static long mock_event_index(const mock *m, ccord_id guild_id, ccord_id event_id)
{
  for (size_t i = 0; i < m->nevents; i++) {
    if (m->events[i].guild_id == guild_id && m->events[i].id == event_id) return (long)i;
  }
  return -1;
}

static ccord_status queue_scheduled_event_update(mock *m, const ccord_scheduled_event *ev, bool deleted,
                                                 ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_SCHEDULED_EVENT_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
  it.events_own = ccord_alloc_array(1, sizeof *it.events_own);
  if (!it.events_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.events_own = *ev;
  it.ev.guild_id = ev->guild_id;
  it.ev.channel_id = ev->channel_id;
  it.ev.scheduled_events = it.events_own;
  it.ev.scheduled_event_count = 1;
  it.ev.scheduled_event_id = ev->id;
  it.ev.scheduled_event_deleted = deleted;
  return mk_push_item(m, &it, err);
}

static bool mock_event_entity_valid(mock *m, ccord_id g, const ccord_scheduled_event *ev, bool check_create_perms,
                                    ccord_status *fail_code, const char **fail_msg)
{
  if (ev->scheduled_start_ms <= 0) {
    *fail_code = CCORD_ERR_RANGE;
    *fail_msg = "Invalid scheduled start time.";
    return false;
  }
  if (ev->entity_type == CCORD_SCHEDULED_EVENT_EXTERNAL) {
    if (ev->channel_id != CCORD_ID_NONE || ev->location[0] == '\0' || ev->scheduled_end_ms <= ev->scheduled_start_ms) {
      *fail_code = CCORD_ERR_INVALID;
      *fail_msg = "External events require a location, an end time after start, and no channel.";
      return false;
    }
    return true;
  }
  if (ev->entity_type != CCORD_SCHEDULED_EVENT_STAGE_INSTANCE && ev->entity_type != CCORD_SCHEDULED_EVENT_VOICE) {
    *fail_code = CCORD_ERR_INVALID;
    *fail_msg = "Invalid scheduled event entity type.";
    return false;
  }
  if (ev->location[0] != '\0' || (ev->scheduled_end_ms != 0 && ev->scheduled_end_ms <= ev->scheduled_start_ms)) {
    *fail_code = CCORD_ERR_INVALID;
    *fail_msg = "Invalid scheduled event configuration.";
    return false;
  }
  size_t ci = mk_admin_channel_index(m, ev->channel_id);
  ccord_channel_kind need_kind = ev->entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE
                                   ? CCORD_CHANNEL_STAGE
                                   : CCORD_CHANNEL_VOICE;
  if (ci == SIZE_MAX || m->channels[ci].guild_id != g || m->channels[ci].kind != need_kind) {
    *fail_code = CCORD_ERR_INVALID;
    *fail_msg = "Invalid event channel.";
    return false;
  }
  if (check_create_perms) {
    uint64_t ch_perms = mock_channel_perms(m, g, ev->channel_id);
    uint64_t need = ev->entity_type == CCORD_SCHEDULED_EVENT_STAGE_INSTANCE
                      ? CCORD_PERM_MUTE_MEMBERS
                      : (CCORD_PERM_VIEW_CHANNEL | CCORD_PERM_CONNECT);
    if ((ch_perms & need) != need) {
      *fail_code = CCORD_ERR_PERMISSION;
      *fail_msg = "You don't have permission to do that. (Missing Permissions)";
      return false;
    }
  }
  return true;
}

bool mk_admin_scheduled_events(mock *m, const ccord_admin_request *req, ccord_status *out, ccord_error *err)
{
  ccord_id g = req->guild_id;
  if (mock_guild_index(m, g) == SIZE_MAX) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
    return true;
  }
  if (mk_admin_member_index(m, g, m->self_id) < 0) {
    *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
    return true;
  }
  uint64_t g_perms = mock_self_perms(m, g);
  switch (req->op) {
    case CCORD_ADMIN_EVENT_LIST: {
      size_t count = 0;
      for (size_t i = 0; i < m->nevents; i++) {
        if (m->events[i].guild_id == g) count++;
      }
      mock_item it = mk_make_item(CCORD_NET_SCHEDULED_EVENT_LIST, mk_now_of(m) + m->cfg.latency_ms);
      it.events_own = ccord_alloc_array(count ? count : 1u, sizeof *it.events_own);
      if (!it.events_own) {
        *out = ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        return true;
      }
      size_t k = 0;
      for (size_t i = 0; i < m->nevents; i++) {
        if (m->events[i].guild_id == g) it.events_own[k++] = m->events[i];
      }
      it.ev.guild_id = g;
      it.ev.scheduled_events = it.events_own;
      it.ev.scheduled_event_count = count;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    case CCORD_ADMIN_EVENT_CREATE: {
      if ((g_perms & (CCORD_PERM_CREATE_EVENTS | CCORD_PERM_MANAGE_EVENTS)) == 0u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_status fcode = CCORD_OK;
      const char *fmsg = nullptr;
      if (!mock_event_entity_valid(m, g, &req->event, true, &fcode, &fmsg)) {
        *out = mk_admin_reply_failed(m, req, fcode, fmsg, err);
        return true;
      }
      if (m->nevents >= MOCK_EVENTS_MAX) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Maximum number of scheduled events reached.", err);
        return true;
      }
      if (m->next_event_id == 0) m->next_event_id = MOCK_EVENT_ID_BASE;
      ccord_scheduled_event ev = req->event;
      ev.id = m->next_event_id++;
      ev.guild_id = g;
      ev.creator_id = m->self_id;
      ev.privacy_level = 2;
      ev.status = CCORD_SCHEDULED_EVENT_SCHEDULED;
      ev.user_count = 0;
      ev.me_rsvped = false;
      m->events[m->nevents++] = ev;
      *out = mock_audit_add(m, g, 100, ev.id, ev.name, req->reason, "");
      if (*out == CCORD_OK) *out = queue_scheduled_event_update(m, &m->events[m->nevents - 1u], false, err);
      return true;
    }
    case CCORD_ADMIN_EVENT_MODIFY: {
      long ei = mock_event_index(m, g, req->event_id);
      if (ei < 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Scheduled Event", err);
        return true;
      }
      bool can_manage = (g_perms & CCORD_PERM_MANAGE_EVENTS) != 0u ||
                        (m->events[ei].creator_id == m->self_id && (g_perms & CCORD_PERM_CREATE_EVENTS) != 0u);
      if (!can_manage) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_scheduled_event next = m->events[ei];
      uint32_t has = req->event_has;
      if (has == 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Nothing to modify.", err);
        return true;
      }
      if (has & CCORD_ADMIN_EVENT_HAS_NAME) memcpy(next.name, req->event.name, sizeof next.name);
      if (has & CCORD_ADMIN_EVENT_HAS_DESCRIPTION) memcpy(next.description, req->event.description, sizeof next.description);
      if (has & CCORD_ADMIN_EVENT_HAS_LOCATION) memcpy(next.location, req->event.location, sizeof next.location);
      if (has & CCORD_ADMIN_EVENT_HAS_CHANNEL) next.channel_id = req->event.channel_id;
      if (has & CCORD_ADMIN_EVENT_HAS_START) next.scheduled_start_ms = req->event.scheduled_start_ms;
      if (has & CCORD_ADMIN_EVENT_HAS_END) next.scheduled_end_ms = req->event.scheduled_end_ms;
      if (has & CCORD_ADMIN_EVENT_HAS_PRIVACY) next.privacy_level = req->event.privacy_level;
      if (has & CCORD_ADMIN_EVENT_HAS_ENTITY_TYPE) next.entity_type = req->event.entity_type;
      if (has & CCORD_ADMIN_EVENT_HAS_STATUS) {
        uint8_t cur = next.status;
        uint8_t dst = req->event.status;
        bool valid_step = (dst == cur) ||
                          (cur == CCORD_SCHEDULED_EVENT_SCHEDULED &&
                           (dst == CCORD_SCHEDULED_EVENT_ACTIVE || dst == CCORD_SCHEDULED_EVENT_CANCELED)) ||
                          (cur == CCORD_SCHEDULED_EVENT_ACTIVE && dst == CCORD_SCHEDULED_EVENT_COMPLETED);
        if (!valid_step) {
          *out = mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid scheduled event status transition.", err);
          return true;
        }
        next.status = dst;
      }
      ccord_status fcode = CCORD_OK;
      const char *fmsg = nullptr;
      if (!mock_event_entity_valid(m, g, &next, (has & (CCORD_ADMIN_EVENT_HAS_CHANNEL | CCORD_ADMIN_EVENT_HAS_ENTITY_TYPE)) != 0u,
                                   &fcode, &fmsg)) {
        *out = mk_admin_reply_failed(m, req, fcode, fmsg, err);
        return true;
      }
      m->events[ei] = next;
      *out = mock_audit_add(m, g, 101, next.id, next.name, req->reason, "");
      if (*out == CCORD_OK) *out = queue_scheduled_event_update(m, &m->events[ei], false, err);
      return true;
    }
    case CCORD_ADMIN_EVENT_DELETE: {
      long ei = mock_event_index(m, g, req->event_id);
      if (ei < 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Scheduled Event", err);
        return true;
      }
      bool can_manage = (g_perms & CCORD_PERM_MANAGE_EVENTS) != 0u ||
                        (m->events[ei].creator_id == m->self_id && (g_perms & CCORD_PERM_CREATE_EVENTS) != 0u);
      if (!can_manage) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      ccord_scheduled_event removed = m->events[ei];
      memmove(&m->events[ei], &m->events[ei + 1], (m->nevents - (size_t)ei - 1u) * sizeof m->events[0]);
      m->nevents--;
      *out = mock_audit_add(m, g, 102, removed.id, removed.name, req->reason, "");
      if (*out == CCORD_OK) *out = queue_scheduled_event_update(m, &removed, true, err);
      return true;
    }
    case CCORD_ADMIN_EVENT_RSVP_ADD:
    case CCORD_ADMIN_EVENT_RSVP_REMOVE: {
      long ei = mock_event_index(m, g, req->event_id);
      if (ei < 0) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Scheduled Event", err);
        return true;
      }
      ccord_id ev_ch = m->events[ei].channel_id;
      uint64_t view_perms = ev_ch != CCORD_ID_NONE ? mock_channel_perms(m, g, ev_ch) : g_perms;
      if ((view_perms & CCORD_PERM_VIEW_CHANNEL) == 0u) {
        *out = mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        return true;
      }
      bool adding = (req->op == CCORD_ADMIN_EVENT_RSVP_ADD);
      if (adding && !m->events[ei].me_rsvped) {
        m->events[ei].me_rsvped = true;
        if (m->events[ei].user_count < UINT32_MAX) m->events[ei].user_count++;
      } else if (!adding && m->events[ei].me_rsvped) {
        m->events[ei].me_rsvped = false;
        if (m->events[ei].user_count > 0) m->events[ei].user_count--;
      }
      mock_item it = mk_make_item(CCORD_NET_SCHEDULED_EVENT_USER_CHANGE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = g;
      it.ev.scheduled_event_id = m->events[ei].id;
      it.ev.user_id = m->self_id;
      it.ev.scheduled_event_user_added = adding;
      *out = mk_push_item(m, &it, err);
      return true;
    }
    default:
      return false;
  }
}

