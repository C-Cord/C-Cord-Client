// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


static const ccord_discover_guild k_mock_discoverable[] = {
  {
    .guild_id = 9001,
    .name = "Linux Gaming Hub",
    .description = "Proton, Vulkan, SDL3, and native Linux gaming discussions.",
    .icon_url = "",
    .vanity_code = "linux-gaming",
    .primary_category_id = 1,
    .online_count = 1420,
    .member_count = 8900,
  },
  {
    .guild_id = 9002,
    .name = "Synthwave & Chiptune",
    .description = "Retro trackers, FM synthesis, and nightly listening parties.",
    .icon_url = "",
    .vanity_code = "synthwave",
    .primary_category_id = 2,
    .online_count = 610,
    .member_count = 4300,
  },
  {
    .guild_id = 9003,
    .name = "Systems Programming Study",
    .description = "C23, OS kernels, compilers, and low-level computer science.",
    .icon_url = "",
    .vanity_code = "c23-study",
    .primary_category_id = 3,
    .online_count = 890,
    .member_count = 6100,
  },
  {
    .guild_id = 9004,
    .name = "Vulkan & Graphics Lab",
    .description = "Real-time rendering, shaders, GPU profiling, and Wayland.",
    .icon_url = "",
    .vanity_code = "vulkan-lab",
    .primary_category_id = 4,
    .online_count = 530,
    .member_count = 3200,
  },
  {
    .guild_id = 9005,
    .name = "Pixel Cinema Club",
    .description = "Indie animation, demoscene showcases, and weekend watchalongs.",
    .icon_url = "",
    .vanity_code = "pixel-cinema",
    .primary_category_id = 5,
    .online_count = 340,
    .member_count = 2100,
  },
};
#define MOCK_DISCOVERABLE_COUNT (sizeof k_mock_discoverable / sizeof k_mock_discoverable[0])


static ccord_status admin_set_guild_folders(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  // Update mock's own guild metadata/order to match what the user persisted.
  ccord_guild reordered[MOCK_GUILDS_MAX];
  bool placed[MOCK_GUILDS_MAX] = {false};
  size_t out_n = 0;
  for (size_t i = 0; i < req->folder_item_count; i++) {
    const ccord_guild_folder_item *fi = &req->folder_items[i];
    for (size_t gi = 0; gi < m->nguilds; gi++) {
      if (!placed[gi] && m->guilds[gi].id == fi->guild_id) {
        ccord_guild g = m->guilds[gi];
        g.folder_id = fi->folder_id;
        memset(g.folder_name, 0, sizeof g.folder_name);
        if (fi->folder_id != 0) {
          (void)snprintf(g.folder_name, sizeof g.folder_name, "%s", fi->folder_name);
          g.folder_color = fi->folder_color;
        } else {
          g.folder_color = 0;
        }
        reordered[out_n++] = g;
        placed[gi] = true;
        break;
      }
    }
  }
  for (size_t gi = 0; gi < m->nguilds; gi++) {
    if (!placed[gi]) {
      ccord_guild g = m->guilds[gi];
      g.folder_id = 0;
      memset(g.folder_name, 0, sizeof g.folder_name);
      g.folder_color = 0;
      reordered[out_n++] = g;
    }
  }
  memcpy(m->guilds, reordered, out_n * sizeof m->guilds[0]);

  mock_item it = mk_make_item(CCORD_NET_GUILD_LAYOUT, mk_now_of(m) + m->cfg.latency_ms);
  size_t n = req->folder_item_count;
  if (n > 0) {
    it.folder_items_own = ccord_alloc_array(n, sizeof *it.folder_items_own);
    if (!it.folder_items_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    memcpy(it.folder_items_own, req->folder_items, n * sizeof *it.folder_items_own);
    it.ev.folder_items = it.folder_items_own;
    it.ev.folder_item_count = n;
  }
  return mk_push_item(m, &it, err);
}

static ccord_status admin_handle_invite(mock *m, const ccord_admin_request *req, bool accept, ccord_error *err)
{
  ccord_invite_preview prev = {0};
  bool found = false;

  for (size_t i = 0; i < m->ninvites; i++) {
    if (mock_str_eq_ci(m->invites[i].inv.code, req->code)) {
      ccord_id gid = m->invites[i].guild_id;
      const char *gname = "Server";
      const char *gicon = "";
      for (size_t gi = 0; gi < m->nguilds; gi++) {
        if (m->guilds[gi].id == gid) {
          gname = m->guilds[gi].name ? m->guilds[gi].name : "Server";
          gicon = m->guilds[gi].icon_url ? m->guilds[gi].icon_url : "";
          break;
        }
      }
      (void)snprintf(prev.code, sizeof prev.code, "%s", m->invites[i].inv.code);
      prev.guild_id = gid;
      (void)snprintf(prev.guild_name, sizeof prev.guild_name, "%s", gname);
      (void)snprintf(prev.guild_icon_url, sizeof prev.guild_icon_url, "%s", gicon);
      prev.channel_id = m->invites[i].inv.channel_id;
      (void)snprintf(prev.channel_name, sizeof prev.channel_name, "%s",
                     m->invites[i].inv.channel_name ? m->invites[i].inv.channel_name : "general");
      prev.online_count = 8u;
      prev.member_count = MOCK_NUM_USERS;
      found = true;
      break;
    }
  }

  for (size_t gi = 0; !found && gi < m->nguilds; gi++) {
    if (m->guilds[gi].vanity_url_code[0] != '\0' && mock_str_eq_ci(m->guilds[gi].vanity_url_code, req->code)) {
      (void)snprintf(prev.code, sizeof prev.code, "%s", m->guilds[gi].vanity_url_code);
      prev.guild_id = m->guilds[gi].id;
      (void)snprintf(prev.guild_name, sizeof prev.guild_name, "%s",
                     m->guilds[gi].name ? m->guilds[gi].name : "Server");
      (void)snprintf(prev.guild_icon_url, sizeof prev.guild_icon_url, "%s",
                     m->guilds[gi].icon_url ? m->guilds[gi].icon_url : "");
      prev.channel_id = CCORD_ID_NONE;
      (void)snprintf(prev.channel_name, sizeof prev.channel_name, "general");
      for (size_t ci = 0; ci < m->nchannels; ci++) {
        if (m->channels[ci].guild_id == m->guilds[gi].id && m->channels[ci].kind == CCORD_CHANNEL_TEXT) {
          prev.channel_id = m->channels[ci].id;
          if (m->channels[ci].name) (void)snprintf(prev.channel_name, sizeof prev.channel_name, "%s", m->channels[ci].name);
          break;
        }
      }
      prev.online_count = 8u;
      prev.member_count = MOCK_NUM_USERS;
      found = true;
    }
  }

  for (size_t i = 0; !found && i < MOCK_DISCOVERABLE_COUNT; i++) {
    const ccord_discover_guild *d = &k_mock_discoverable[i];
    if (mock_str_eq_ci(d->vanity_code, req->code)) {
      (void)snprintf(prev.code, sizeof prev.code, "%s", d->vanity_code);
      prev.guild_id = d->guild_id;
      (void)snprintf(prev.guild_name, sizeof prev.guild_name, "%s", d->name);
      (void)snprintf(prev.guild_icon_url, sizeof prev.guild_icon_url, "%s", d->icon_url);
      (void)snprintf(prev.guild_description, sizeof prev.guild_description, "%s", d->description);
      prev.channel_id = CCORD_ID_NONE;
      for (size_t ci = 0; ci < m->nchannels; ci++) {
        if (m->channels[ci].guild_id == d->guild_id && m->channels[ci].kind == CCORD_CHANNEL_TEXT) {
          prev.channel_id = m->channels[ci].id;
          break;
        }
      }
      (void)snprintf(prev.channel_name, sizeof prev.channel_name, "general");
      prev.online_count = d->online_count;
      prev.member_count = d->member_count;
      found = true;
    }
  }

  if (!found && mock_str_eq_ci(req->code, MOCK_MV_INVITE_CODE)) {
    (void)snprintf(prev.code, sizeof prev.code, "%s", MOCK_MV_INVITE_CODE);
    prev.guild_id = MOCK_MV_GUILD_ID;
    (void)snprintf(prev.guild_name, sizeof prev.guild_name, "%s", MOCK_MV_GUILD_NAME);
    (void)snprintf(prev.guild_description, sizeof prev.guild_description, "A server with membership screening.");
    prev.channel_id = CCORD_ID_NONE;
    (void)snprintf(prev.channel_name, sizeof prev.channel_name, "general");
    prev.online_count = 4u;
    prev.member_count = 12u;
    found = true;
  }

  if (!found) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Invite", err);

  if (accept && !mock_guild_known(m, prev.guild_id)) {
    ccord_id mat_gid = CCORD_ID_NONE, mat_cid = CCORD_ID_NONE;
    ccord_status mst = mock_materialize_guild(m, prev.guild_id, prev.guild_name, false,
                                              prev.guild_id == MOCK_MV_GUILD_ID, CCORD_ADMIN_NONE,
                                              &mat_gid, &mat_cid, err);
    if (mst != CCORD_OK) return mst;
    prev.channel_id = mat_cid;
  }

  mock_item it = mk_make_item(accept ? CCORD_NET_INVITE_ACCEPTED : CCORD_NET_INVITE_PREVIEW,
                           mk_now_of(m) + m->cfg.latency_ms);
  it.invite_preview_own = ccord_alloc_array(1, sizeof *it.invite_preview_own);
  if (!it.invite_preview_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.invite_preview_own = prev;
  it.ev.guild_id = prev.guild_id;
  it.ev.channel_id = prev.channel_id;
  it.ev.invite_preview = it.invite_preview_own;
  return mk_push_item(m, &it, err);
}

// GET /guilds/{g}/member-verification: only the screening guild has a form (Discord answers 404 for the rest).
static ccord_status admin_member_verification_get(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  if (!mock_guild_known(m, req->guild_id) || req->guild_id != MOCK_MV_GUILD_ID) {
    return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Member Verification Form", err);
  }
  mock_item it = mk_make_item(CCORD_NET_MEMBER_VERIFICATION, mk_now_of(m) + m->cfg.latency_ms);
  it.mv_own = ccord_alloc_array(1, sizeof *it.mv_own);
  if (!it.mv_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  *it.mv_own = (ccord_member_verification){
    .version = "2026-11-14T00:00:00.000000+00:00",
    .description = "Welcome to " MOCK_MV_GUILD_NAME "! Read the rules and accept them to start chatting.",
    .field_count = 1u,
  };
  it.mv_own->fields[0] = (ccord_member_verification_field){
    .field_type = CCORD_MEMBER_VERIFICATION_TERMS,
    .label = "Server rules",
    .values = {"Be kind and respectful to everyone.", "No spam, scams or self-promotion.",
               "Keep each conversation in the right channel."},
    .value_count = 3u,
    .required = true,
  };
  it.ev.guild_id = req->guild_id;
  it.ev.member_verification = it.mv_own;
  return mk_push_item(m, &it, err);
}

// PUT /guilds/{g}/requests/@me: the self member stops being pending once the required TERMS field is accepted
// ("response":true in the submitted form), and the gateway-style member update says so. A form that does not accept
// the rules is refused like Discord's 400 and leaves the member pending.
static ccord_status admin_member_verification_submit(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  long mi = mock_guild_known(m, req->guild_id) ? mk_admin_member_index(m, req->guild_id, m->self_id) : -1;
  if (mi < 0) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
  if (req->guild_id != MOCK_MV_GUILD_ID) {
    return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild Member Verification Form", err);
  }
  if (!strstr(req->body, "\"response\":true")) {
    return mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Required form fields were not answered", err);
  }
  if (!m->members[mi].pending) return CCORD_OK;   // already through screening: nothing to confirm
  m->members[mi].pending = false;
  return mk_admin_emit_member(m, (size_t)mi, err);
}

ccord_status mock_request_discoverable_guilds(ccord_transport *t, const char *query, uint32_t category_id,
                                                     uint32_t offset, uint32_t limit, ccord_error *err)
{
  mock *m = (mock *)t;
  m->calls[CCORD_MOCK_CALL_DISCOVERY]++;
  ccord_error_clear(err);
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  const char *q = query ? query : "";
  size_t qlen = strlen(q);
  if (qlen > 100u || !ccord_utf8_valid_text(q, qlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: invalid discovery query");
  }
  if (limit == 0 || limit > CCORD_DISCOVERY_PAGE_MAX) limit = CCORD_DISCOVERY_PAGE_MAX;

  mock_item it = mk_make_item(CCORD_NET_DISCOVERY_RESULTS, mk_now_of(m) + m->cfg.latency_ms);
  it.discovery_own = ccord_alloc_array(limit, sizeof *it.discovery_own);
  it.discovery_query_own = ccord_strndup(q, qlen);
  if (!it.discovery_own || !it.discovery_query_own) {
    mk_item_release(&it);
    return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  }

  size_t matched = 0, out_n = 0;
  for (size_t i = 0; i < MOCK_DISCOVERABLE_COUNT; i++) {
    const ccord_discover_guild *d = &k_mock_discoverable[i];
    if (category_id != 0 && d->primary_category_id != category_id) continue;
    if (qlen > 0 && !mk_has_ci_substr(d->name, q, qlen) && !mk_has_ci_substr(d->description, q, qlen)) continue;
    if (matched >= offset && out_n < limit) {
      it.discovery_own[out_n++] = *d;
    }
    matched++;
  }

  it.ev.discovery_guilds = it.discovery_own;
  it.ev.discovery_count = out_n;
  it.ev.discovery_total = (uint32_t)matched;
  it.ev.discovery_offset = offset;
  it.ev.discovery_category_id = category_id;
  it.ev.discovery_query = it.discovery_query_own;
  return mk_push_item(m, &it, err);
}

// Notification settings are the user's own (no permission involved): the mock keeps the row, then answers with the
// server's whole set, as the gateway's USER_GUILD_SETTINGS_UPDATE does.
static ccord_status admin_set_notify(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  ccord_id g = req->guild_id;
  bool known = g == CCORD_DM_GUILD_ID;
  for (size_t i = 0; !known && i < m->nguilds; i++) known = m->guilds[i].id == g;
  if (!known) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
  if (req->channel_id != CCORD_ID_NONE && mk_admin_channel_index(m, req->channel_id) == SIZE_MAX) {
    return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
  }
  size_t at = SIZE_MAX, last_of_guild = SIZE_MAX;
  for (size_t i = 0; i < m->nnotify; i++) {
    if (m->notify[i].guild_id != g) continue;
    last_of_guild = i;
    if (m->notify[i].channel_id == req->channel_id) at = i;
  }
  if (at == SIZE_MAX) {
    if (m->nnotify == MOCK_NOTIFY_MAX) return mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Too many overrides", err);
    at = last_of_guild == SIZE_MAX ? m->nnotify : last_of_guild + 1u;   // rows stay grouped by server
    memmove(&m->notify[at + 1], &m->notify[at], (m->nnotify - at) * sizeof m->notify[0]);
    m->nnotify++;
    m->notify[at] = (ccord_notify_scope){.guild_id = g, .channel_id = req->channel_id, .level = CCORD_NOTIFY_INHERIT};
  }
  ccord_notify_scope *r = &m->notify[at];
  if (req->notify_muted >= 0) {
    r->muted = req->notify_muted == 1;
    r->mute_until_ms = r->muted ? req->until_ms : 0;
  }
  if (req->notify_level >= 0) r->level = (ccord_notify_level)req->notify_level;
  if (req->notify_suppress_everyone >= 0) r->suppress_everyone = req->notify_suppress_everyone == 1;
  if (req->notify_suppress_roles >= 0) r->suppress_roles = req->notify_suppress_roles == 1;
  mock_item it = mk_make_item(CCORD_NET_NOTIFY_SETTINGS, mk_now_of(m) + m->cfg.latency_ms);
  size_t n = 0;
  for (size_t i = 0; i < m->nnotify; i++) n += m->notify[i].guild_id == g;
  it.notify_own = ccord_alloc_array(n ? n : 1u, sizeof *it.notify_own);
  if (!it.notify_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
  size_t k = 0;
  for (size_t i = 0; i < m->nnotify; i++) {
    if (m->notify[i].guild_id == g) it.notify_own[k++] = m->notify[i];
  }
  it.ev.guild_id = g;
  it.ev.notify = it.notify_own;
  it.ev.notify_count = k;
  return mk_push_item(m, &it, err);
}

static void mock_default_group_dm_name(const mock *m, const ccord_id *recipient_ids, size_t recipient_count,
                                       char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  size_t pos = 0;
  for (size_t i = 0; i < recipient_count; i++) {
    const char *nm = mock_user_name(m, recipient_ids[i]);
    if (!nm || nm[0] == '\0') continue;
    if (pos > 0 && pos + 2u < cap) {
      out[pos++] = ',';
      out[pos++] = ' ';
      out[pos] = '\0';
    }
    size_t nlen = strlen(nm);
    for (size_t k = 0; k < nlen && pos + 1u < cap && pos < CCORD_NAME_MAX_BYTES;) {
      unsigned char c = (unsigned char)nm[k];
      size_t seq = c < 0x80u ? 1u : (c >= 0xF0u ? 4u : (c >= 0xE0u ? 3u : (c >= 0xC0u ? 2u : 1u)));
      if (pos + seq >= cap || pos + seq > CCORD_NAME_MAX_BYTES || k + seq > nlen) break;
      memcpy(out + pos, nm + k, seq);
      pos += seq;
      k += seq;
    }
    out[pos] = '\0';
  }
  if (out[0] == '\0') (void)snprintf(out, cap, "Group DM");
}

// ---- embedded activities (GET /activities/shelf, POST /activities/{c}/{app}, POST .../instances/{i}/leave) -------

// The shelf every mock guild offers, in shelf order (rank 1 first). Application ids sit in their own range so they
// never collide with the mock's user, channel, role or message ids.
#define MOCK_ACTIVITY_APP_BASE 97000u
static const ccord_activity_app k_mock_activity_apps[] = {
  {.application_id = MOCK_ACTIVITY_APP_BASE + 1u, .name = "Sketch Heads",
   .description = "Draw and guess together: one sketches, everyone else races to name it.",
   .icon_hash = "mock-sketch-icon", .cover_hash = "mock-sketch-cover", .max_participants = 8u, .shelf_rank = 1u},
  {.application_id = MOCK_ACTIVITY_APP_BASE + 2u, .name = "Poker Night",
   .description = "Texas Hold'em at a shared table, with chips on the house.",
   .icon_hash = "mock-poker-icon", .cover_hash = "mock-poker-cover", .max_participants = 7u, .shelf_rank = 2u},
  {.application_id = MOCK_ACTIVITY_APP_BASE + 3u, .name = "Watch Together",
   .description = "Queue up videos and watch them in sync with the whole channel.",
   .icon_hash = "mock-watch-icon", .cover_hash = "", .max_participants = 25u, .shelf_rank = 3u},
};
#define MOCK_ACTIVITY_APP_COUNT (sizeof k_mock_activity_apps / sizeof k_mock_activity_apps[0])

static const ccord_activity_app *mock_activity_app(ccord_id application_id)
{
  for (size_t i = 0; i < MOCK_ACTIVITY_APP_COUNT; i++) {
    if (k_mock_activity_apps[i].application_id == application_id) return &k_mock_activity_apps[i];
  }
  return nullptr;
}

static long mock_activity_by_instance(const mock *m, ccord_id instance_id)
{
  for (size_t i = 0; i < m->nactivities; i++) {
    if (m->activities[i].instance_id == instance_id) return (long)i;
  }
  return -1;
}

static long mock_activity_by_app(const mock *m, ccord_id channel_id, ccord_id application_id)
{
  for (size_t i = 0; i < m->nactivities; i++) {
    if (m->activities[i].channel_id == channel_id && m->activities[i].application_id == application_id) {
      return (long)i;
    }
  }
  return -1;
}

static long mock_activity_participant(const ccord_activity_instance *inst, ccord_id user_id)
{
  for (size_t i = 0; i < inst->participant_count; i++) {
    if (inst->participants[i] == user_id) return (long)i;
  }
  return -1;
}

// Drops row `i` from the registry and queues the end-of-instance update (participant_count 0).
static ccord_status mock_activity_end(mock *m, size_t i, uint64_t due, ccord_error *err)
{
  ccord_activity_instance ended = m->activities[i];
  ended.participant_count = 0;
  memset(ended.participants, 0, sizeof ended.participants);
  memmove(&m->activities[i], &m->activities[i + 1u], (m->nactivities - i - 1u) * sizeof m->activities[0]);
  m->nactivities--;
  return mk_queue_activity_instance(m, &ended, due, err);
}

static ccord_status admin_activity(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  switch (req->op) {
    case CCORD_ADMIN_ACTIVITY_SHELF: {
      if (mock_guild_index(m, req->guild_id) == SIZE_MAX) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Guild", err);
      }
      mock_item it = mk_make_item(CCORD_NET_ACTIVITY_SHELF, due);
      it.activity_apps_own = ccord_alloc_array(MOCK_ACTIVITY_APP_COUNT, sizeof *it.activity_apps_own);
      if (!it.activity_apps_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      memcpy(it.activity_apps_own, k_mock_activity_apps, sizeof k_mock_activity_apps);
      it.ev.guild_id = req->guild_id;
      it.ev.activity_apps = it.activity_apps_own;
      it.ev.activity_app_count = MOCK_ACTIVITY_APP_COUNT;
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_ACTIVITY_LAUNCH: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Channel", err);
      const ccord_channel *ch = &m->channels[ci];
      if (ch->kind != CCORD_CHANNEL_VOICE) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Activities run in voice channels.", err);
      }
      long vi = mock_voice_index(m, ch->guild_id, m->self_id);
      if (vi < 0 || m->voices[vi].channel_id != ch->id) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Join the voice channel before starting an activity.", err);
      }
      if ((mock_channel_perms(m, ch->guild_id, ch->id) & CCORD_PERM_USE_EMBEDDED_ACTIVITIES) == 0u) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      }
      const ccord_activity_app *app = mock_activity_app(req->application_id);
      if (!app) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Application", err);
      // Launching an app that already runs in the channel joins that instance (Discord answers the same way).
      long ai = mock_activity_by_app(m, ch->id, app->application_id);
      if (ai < 0) {
        if (m->nactivities >= MOCK_ACTIVITIES_MAX) {
          return mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Too many running activities.", err);
        }
        if (m->next_admin_id == 0) m->next_admin_id = 8000;
        ai = (long)m->nactivities++;
        m->activities[ai] = (ccord_activity_instance){.instance_id = m->next_admin_id++,
                                                      .application_id = app->application_id,
                                                      .guild_id = ch->guild_id,
                                                      .channel_id = ch->id,
                                                      .location_id = ch->id};
      }
      ccord_activity_instance *inst = &m->activities[ai];
      if (mock_activity_participant(inst, m->self_id) < 0) {
        uint32_t cap = app->max_participants ? app->max_participants : CCORD_ACTIVITY_MAX_PARTICIPANTS;
        if (inst->participant_count >= cap || inst->participant_count >= CCORD_ACTIVITY_MAX_PARTICIPANTS) {
          return mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "This activity is full.", err);
        }
        inst->participants[inst->participant_count++] = m->self_id;
      }
      return mk_queue_activity_instance(m, inst, due, err);
    }
    case CCORD_ADMIN_ACTIVITY_LEAVE: {
      long ai = mock_activity_by_instance(m, req->instance_id);
      if (ai < 0 || m->activities[ai].application_id != req->application_id) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Activity Instance", err);
      }
      ccord_activity_instance *inst = &m->activities[ai];
      long pi = mock_activity_participant(inst, m->self_id);
      if (pi >= 0) {
        memmove(&inst->participants[pi], &inst->participants[pi + 1], (inst->participant_count - (size_t)pi - 1u) *
                                                                          sizeof inst->participants[0]);
        inst->participant_count--;
        inst->participants[inst->participant_count] = CCORD_ID_NONE;
      }
      if (inst->participant_count == 0) return mock_activity_end(m, (size_t)ai, due, err);   // the last one out ends it
      return mk_queue_activity_instance(m, inst, due, err);
    }
    default:
      return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not an activity request");
  }
}

ccord_status ccord_mock_push_activity_instance(ccord_transport *t, ccord_id channel_id, ccord_id application_id,
                                               const ccord_id *participants, size_t count, ccord_id *out_instance_id,
                                               ccord_error *err)
{
  ccord_error_clear(err);
  if (!t || (!participants && count > 0) || count > CCORD_ACTIVITY_MAX_PARTICIPANTS) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad activity arguments");
  }
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  size_t ci = mk_admin_channel_index(m, channel_id);
  if (ci == SIZE_MAX || m->channels[ci].kind != CCORD_CHANNEL_VOICE) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown voice channel");
  }
  if (!mock_activity_app(application_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown activity app");
  for (size_t i = 0; i < count; i++) {
    if (participants[i] == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad participant");
  }
  uint64_t due = mk_now_of(m) + m->cfg.latency_ms;
  long ai = mock_activity_by_app(m, channel_id, application_id);
  if (ai >= 0 && count == 0) {   // an empty list ends the running instance
    if (out_instance_id) *out_instance_id = m->activities[ai].instance_id;
    return mock_activity_end(m, (size_t)ai, due, err);
  }
  if (count == 0) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: no such activity instance");
  if (ai < 0) {
    if (m->nactivities >= MOCK_ACTIVITIES_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "mock: too many activities");
    if (m->next_admin_id == 0) m->next_admin_id = 8000;
    ai = (long)m->nactivities++;
    m->activities[ai] = (ccord_activity_instance){.instance_id = m->next_admin_id++,
                                                  .application_id = application_id,
                                                  .guild_id = m->channels[ci].guild_id,
                                                  .channel_id = channel_id,
                                                  .location_id = channel_id};
  }
  ccord_activity_instance *inst = &m->activities[ai];
  memset(inst->participants, 0, sizeof inst->participants);
  memcpy(inst->participants, participants, count * sizeof participants[0]);
  inst->participant_count = count;
  if (out_instance_id) *out_instance_id = inst->instance_id;
  return mk_queue_activity_instance(m, inst, due, err);
}

static ccord_status admin_guild_profile_get(mock *m, const ccord_admin_request *req, ccord_error *err)
{
  const ccord_guild *g = nullptr;
  for (size_t i = 0; i < m->nguilds; i++) {
    if (m->guilds[i].id == req->guild_id) {
      g = &m->guilds[i];
      break;
    }
  }
  ccord_guild_profile gp = {.guild_id = req->guild_id};
  if (g) {
    (void)snprintf(gp.name, sizeof gp.name, "%s", g->name ? g->name : "Mock Server");
    (void)snprintf(gp.description, sizeof gp.description, "Community server for %s", gp.name);
    if (g->icon_url) (void)snprintf(gp.icon_url, sizeof gp.icon_url, "%s", g->icon_url);
    (void)snprintf(gp.tag, sizeof gp.tag, "%s", (g->id == m->guilds[0].id) ? "CCRD" : "MOCK");
    gp.brand_color = 0x5865F2u;
    gp.has_brand_color = true;
    gp.online_count = 42;
    gp.member_count = 128;
  } else {
    gp.unavailable = true;
  }
  mock_item it = mk_make_item(CCORD_NET_GUILD_PROFILE, mk_now_of(m) + m->cfg.latency_ms);
  it.ev.guild_id = req->guild_id;
  it.ev.guild_profile = gp;
  return mk_push_item(m, &it, err);
}

ccord_status mock_admin_action(ccord_transport *t, const ccord_admin_request *req, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (!req || req->op == CCORD_ADMIN_NONE || req->op >= CCORD_ADMIN_OP_COUNT) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad admin request");
  }
  if (!m->last_admin) m->last_admin = ccord_alloc_array(1, sizeof *m->last_admin);
  if (m->last_admin) *m->last_admin = *req;
  m->admin_count++;
  if (m->fail_next_admin) {   // test hook: the request reached the server and Discord answered with an error
    m->fail_next_admin = false;
    return mk_admin_reply_failed(m, req, CCORD_ERR_INTERNAL, "mock: injected admin failure", err);
  }
  if (m->next_admin_id == 0) m->next_admin_id = 8000;
  if (req->op == CCORD_ADMIN_SET_NOTIFY) return admin_set_notify(m, req, err);   // not a permissioned write
  if (req->op == CCORD_ADMIN_PREVIEW_INVITE) return admin_handle_invite(m, req, false, err);
  if (req->op == CCORD_ADMIN_GET_MEMBER_VERIFICATION) return admin_member_verification_get(m, req, err);
  if (req->op == CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION) return admin_member_verification_submit(m, req, err);   // self-service
  if (req->op == CCORD_ADMIN_EVENT_LIST) {
    ccord_status served_ro = CCORD_OK;
    (void)mk_admin_scheduled_events(m, req, &served_ro, err);
    return served_ro;
  }
  if (req->op == CCORD_ADMIN_ACTIVITY_SHELF) return admin_activity(m, req, err);   // a read: refuse_writes spares it
  if (req->op == CCORD_ADMIN_GET_GUILD_PROFILE) return admin_guild_profile_get(m, req, err);
  if (m->cfg.refuse_writes) {
    return mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, "You don't have permission to do that. (Missing Permissions)",
                              err);
  }
  if (req->op == CCORD_ADMIN_ACTIVITY_LAUNCH || req->op == CCORD_ADMIN_ACTIVITY_LEAVE) {
    return admin_activity(m, req, err);
  }
  if (req->op == CCORD_ADMIN_SET_GUILD_FOLDERS) return admin_set_guild_folders(m, req, err);
  if (req->op == CCORD_ADMIN_CREATE_GUILD) {
    return mock_materialize_guild(m, CCORD_ID_NONE, req->name, true, false, CCORD_ADMIN_CREATE_GUILD, nullptr, nullptr,
                                  err);
  }
  if (req->op == CCORD_ADMIN_ACCEPT_INVITE) return admin_handle_invite(m, req, true, err);
  static const char gone[] = "It no longer exists.";
  ccord_status served = CCORD_OK;
  if (mock_admin_server(m, req, &served, err)) return served;
  switch (req->op) {
    case CCORD_ADMIN_CREATE_CHANNEL:
    case CCORD_ADMIN_CREATE_THREAD:
    case CCORD_ADMIN_CREATE_THREAD_MSG: {
      bool from_msg = req->op == CCORD_ADMIN_CREATE_THREAD_MSG;
      bool thread = req->op == CCORD_ADMIN_CREATE_THREAD || from_msg;
      size_t parent_ci = SIZE_MAX;
      long starter_mi = -1;
      if (from_msg) {
        parent_ci = mk_admin_channel_index(m, req->channel_id);
        if (parent_ci == SIZE_MAX) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
        starter_mi = mk_find_message(m, parent_ci, req->message_id);
        if (starter_mi < 0) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      const char *name = mk_admin_owned(m, req->name);
      if (!name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      size_t in_guild = 0;
      for (size_t i = 0; i < m->nchannels; i++) in_guild += m->channels[i].guild_id == req->guild_id;
      ccord_channel ch = {
        .id = m->next_admin_id++,
        .guild_id = req->guild_id,
        .parent_id = thread ? req->channel_id : req->parent_id,
        .kind = req->channel_type == 13 ? CCORD_CHANNEL_STAGE
                : req->channel_type == 2 ? CCORD_CHANNEL_VOICE
                : req->channel_type == 4                          ? CCORD_CHANNEL_CATEGORY
                                                                  : CCORD_CHANNEL_TEXT,
        .name = name,
        .topic = "",
        .position = (int32_t)in_guild,
        .discord_type = thread ? 11u : req->channel_type,
        .thread_has_member = thread,
        .thread_joined = thread,
        .thread_auto_archive_mins = thread ? 1440u : 0u,
      };
      size_t ci = mk_admin_add_channel(m, &ch, err);
      if (ci == SIZE_MAX) return CCORD_ERR_NOMEM;
      if (from_msg && parent_ci != SIZE_MAX && starter_mi >= 0) {
        ccord_message *smsg = &m->chmsgs[parent_ci][starter_mi];
        ccord_message_rich nr = {0};
        if (smsg->rich) nr = *smsg->rich;
        nr.present |= CCORD_RICH_ALL | CCORD_RICH_HAS_THREAD;
        nr.thread = (ccord_msg_thread){.id = ch.id, .name = name, .message_count = 0};
        ccord_message_rich *cloned = ccord_message_rich_clone(&nr, err);
        if (cloned) {
          mk_retire_rich(m, smsg->rich);
          smsg->rich = cloned;
          (void)mk_queue_message_update(m, *smsg, mk_now_of(m) + m->cfg.latency_ms, err);
        }
      }
      ccord_status as = mock_audit_add(m, req->guild_id, thread ? 110 : 10, ch.id, name, req->reason, "");
      if (as != CCORD_OK) return as;
      return mk_admin_emit_channel(m, ci, err);
    }
    case CCORD_ADMIN_ARCHIVE_THREAD: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || !ccord_discord_type_is_thread(m->channels[ci].discord_type)) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      m->channels[ci].thread_archived = req->thread_archived;
      ccord_status as = mock_audit_add(m, m->channels[ci].guild_id, 111, req->channel_id, m->channels[ci].name,
                                       req->reason, req->thread_archived ? "archived: true" : "archived: false");
      if (as != CCORD_OK) return as;
      return mk_admin_emit_channel(m, ci, err);
    }
    case CCORD_ADMIN_JOIN_THREAD:
    case CCORD_ADMIN_LEAVE_THREAD: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || !ccord_discord_type_is_thread(m->channels[ci].discord_type)) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      bool joined = req->op == CCORD_ADMIN_JOIN_THREAD;
      m->channels[ci].thread_has_member = true;
      m->channels[ci].thread_joined = joined;
      mock_item it = mk_make_item(CCORD_NET_THREAD_MEMBER_SELF, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = m->channels[ci].guild_id;
      it.ev.channel_id = req->channel_id;
      it.ev.user_id = m->self_id;
      it.ev.thread_joined = joined;
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_EDIT_CHANNEL: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      if (m->channels[ci].guild_id != CCORD_DM_GUILD_ID &&
          !ccord_discord_type_is_thread(m->channels[ci].discord_type) &&
          (mock_channel_perms(m, m->channels[ci].guild_id, req->channel_id) & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      }
      if (req->name[0] != '\0') {
        const char *name = mk_admin_owned(m, req->name);
        if (!name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        m->channels[ci].name = name;
      }
      if (req->has_topic) {
        const char *topic = mk_admin_owned(m, req->topic);
        if (!topic) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        m->channels[ci].topic = topic;
      }
      if (req->has_slowmode) {
        m->channels[ci].rate_limit_per_user = req->slowmode_s;
      }
      if (req->has_nsfw) {
        m->channels[ci].nsfw = req->nsfw;
      }
      if (req->has_bitrate) {
        m->channels[ci].bitrate = req->bitrate;
      }
      if (req->has_user_limit) {
        m->channels[ci].user_limit = req->user_limit;
      }
      if (req->has_auto_archive) {
        m->channels[ci].default_auto_archive_duration = req->default_auto_archive_duration;
      }
      if (req->has_available_tags && ccord_discord_type_is_forum(m->channels[ci].discord_type)) {
        size_t fmi = mock_ensure_forum_meta(m, req->channel_id);
        if (fmi != SIZE_MAX) {
          m->forum_metas[fmi].tag_count = req->available_tag_count;
          for (uint8_t k = 0u; k < req->available_tag_count; k++) {
            ccord_forum_tag ftag = req->available_tags[k];
            if (ftag.id == CCORD_ID_NONE) ftag.id = 4100u + (m->next_admin_id++);
            m->forum_metas[fmi].tags[k] = ftag;
          }
        }
      }
      ccord_status as = mock_audit_add(m, m->channels[ci].guild_id, 11, req->channel_id, m->channels[ci].name,
                                       req->reason, req->has_topic ? "topic changed" : "channel updated");
      if (as != CCORD_OK) return as;
      return mk_admin_emit_channel(m, ci, err);
    }
    case CCORD_ADMIN_DELETE_CHANNEL:
    case CCORD_ADMIN_CLOSE_DM:
    case CCORD_ADMIN_REJECT_MESSAGE_REQUEST: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      ccord_id guild = m->channels[ci].guild_id;
      if (req->op == CCORD_ADMIN_DELETE_CHANNEL) {
        if (guild != CCORD_DM_GUILD_ID && !ccord_discord_type_is_thread(m->channels[ci].discord_type) &&
            (mock_channel_perms(m, guild, req->channel_id) & CCORD_PERM_MANAGE_CHANNELS) == 0u) {
          return mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
        }
        ccord_status as = mock_audit_add(m, guild, 12, req->channel_id, m->channels[ci].name, req->reason, "");
        if (as != CCORD_OK) return as;
      }
      mk_admin_remove_channel(m, ci);
      mock_item it = mk_make_item(CCORD_NET_CHANNEL_DELETE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = guild;
      it.ev.channel_id = req->channel_id;
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_ACCEPT_MESSAGE_REQUEST: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].guild_id != CCORD_DM_GUILD_ID) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      m->channels[ci].is_message_request = false;
      m->channels[ci].is_spam = false;
      mock_item it = mk_make_item(CCORD_NET_MESSAGE_REQUEST_ACCEPTED, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = CCORD_DM_GUILD_ID;
      it.ev.channel_id = req->channel_id;
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_CREATE_GROUP_DM: {
      if (req->dm_recipient_count == 0 || req->dm_recipient_count > CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid recipients", err);
      }
      for (size_t i = 0; i < req->dm_recipient_count; i++) {
        ccord_id rid = req->dm_recipient_ids[i];
        if (rid == CCORD_ID_NONE || rid == m->self_id) {
          return mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid recipient", err);
        }
        bool known = false;
        for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
          if (m->users[ui].id == rid) {
            known = true;
            break;
          }
        }
        if (!known) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown User", err);
      }
      if (req->dm_recipient_count == 1u && req->name[0] == '\0') {
        ccord_id rid = req->dm_recipient_ids[0];
        ccord_id dm_cid = 900000u + rid;
        size_t ci = mk_admin_channel_index(m, dm_cid);
        if (ci == SIZE_MAX) {
          const char *uname = "Direct Message";
          const char *uavatar = "";
          for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
            if (m->users[ui].id == rid) {
              uname = m->users[ui].name;
              uavatar = m->users[ui].avatar_url ? m->users[ui].avatar_url : "";
              break;
            }
          }
          ccord_channel dmc = {
            .id = dm_cid,
            .guild_id = CCORD_DM_GUILD_ID,
            .parent_id = CCORD_ID_NONE,
            .kind = CCORD_CHANNEL_TEXT,
            .discord_type = CCORD_DISCORD_CHANNEL_DM,
            .name = uname,
            .topic = "Direct Message",
            .user_id = rid,
            .recipient_count = 1u,
            .recipient_ids = {rid},
            .icon_url = uavatar,
          };
          ci = mk_admin_add_channel(m, &dmc, err);
          if (ci == SIZE_MAX) return CCORD_ERR_NOMEM;
        }
        return mk_admin_emit_channel_op(m, ci, CCORD_ADMIN_CREATE_GROUP_DM, err);
      }
      char def_name[CCORD_NAME_MAX_BYTES + 1];
      const char *raw_name = req->name;
      if (raw_name[0] == '\0') {
        mock_default_group_dm_name(m, req->dm_recipient_ids, req->dm_recipient_count, def_name, sizeof def_name);
        raw_name = def_name;
      }
      const char *owned_name = mk_admin_owned(m, raw_name);
      if (!owned_name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      ccord_channel gdm = {
        .id = m->next_admin_id++,
        .guild_id = CCORD_DM_GUILD_ID,
        .parent_id = CCORD_ID_NONE,
        .kind = CCORD_CHANNEL_TEXT,
        .discord_type = CCORD_DISCORD_CHANNEL_GROUP_DM,
        .name = owned_name,
        .topic = "",
        .user_id = req->dm_recipient_ids[0],
        .owner_id = m->self_id,
        .recipient_count = (uint16_t)req->dm_recipient_count,
        .icon_url = "",
      };
      for (size_t i = 0; i < req->dm_recipient_count; i++) {
        gdm.recipient_ids[i] = req->dm_recipient_ids[i];
      }
      size_t ci = mk_admin_add_channel(m, &gdm, err);
      if (ci == SIZE_MAX) return CCORD_ERR_NOMEM;
      return mk_admin_emit_channel_op(m, ci, CCORD_ADMIN_CREATE_GROUP_DM, err);
    }
    case CCORD_ADMIN_ADD_DM_RECIPIENT: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      if (req->user_id == CCORD_ID_NONE || req->user_id == m->self_id) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid recipient", err);
      }
      const ccord_user *tu = nullptr;
      for (size_t ui = 0; ui < MOCK_NUM_USERS; ui++) {
        if (m->users[ui].id == req->user_id) {
          tu = &m->users[ui];
          break;
        }
      }
      if (!tu) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown User", err);
      ccord_channel *ch = &m->channels[ci];
      bool already = false;
      for (uint16_t i = 0; i < ch->recipient_count; i++) {
        if (ch->recipient_ids[i] == req->user_id) {
          already = true;
          break;
        }
      }
      if (!already) {
        if (ch->recipient_count >= CCORD_ADMIN_GROUP_DM_RECIPIENTS_MAX) {
          return mk_admin_reply_failed(m, req, CCORD_ERR_RANGE, "Group DM has reached maximum recipients", err);
        }
        ch->recipient_ids[ch->recipient_count++] = req->user_id;
        if (ch->user_id == CCORD_ID_NONE) ch->user_id = req->user_id;
      }
      mock_item it = mk_make_item(CCORD_NET_DM_RECIPIENT_CHANGE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = CCORD_DM_GUILD_ID;
      it.ev.channel_id = req->channel_id;
      it.ev.user_id = req->user_id;
      it.ev.recipient_added = true;
      it.ev.users = tu;
      it.ev.user_count = 1u;
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_REMOVE_DM_RECIPIENT: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      ccord_channel *ch = &m->channels[ci];
      if (req->user_id == m->self_id) {
        mk_admin_remove_channel(m, ci);
        mock_item it = mk_make_item(CCORD_NET_CHANNEL_DELETE, mk_now_of(m) + m->cfg.latency_ms);
        it.ev.guild_id = CCORD_DM_GUILD_ID;
        it.ev.channel_id = req->channel_id;
        return mk_push_item(m, &it, err);
      }
      if (ch->owner_id != m->self_id) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_PERMISSION, k_missing_permissions, err);
      }
      size_t at = SIZE_MAX;
      for (uint16_t i = 0; i < ch->recipient_count; i++) {
        if (ch->recipient_ids[i] == req->user_id) {
          at = i;
          break;
        }
      }
      if (at == SIZE_MAX) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, "Unknown Recipient", err);
      memmove(&ch->recipient_ids[at], &ch->recipient_ids[at + 1u],
              (ch->recipient_count - at - 1u) * sizeof ch->recipient_ids[0]);
      ch->recipient_count--;
      ch->recipient_ids[ch->recipient_count] = CCORD_ID_NONE;
      ch->user_id = ch->recipient_count > 0 ? ch->recipient_ids[0] : CCORD_ID_NONE;
      mock_item it = mk_make_item(CCORD_NET_DM_RECIPIENT_CHANGE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = CCORD_DM_GUILD_ID;
      it.ev.channel_id = req->channel_id;
      it.ev.user_id = req->user_id;
      it.ev.recipient_added = false;
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_EDIT_GROUP_DM: {
      size_t ci = mk_admin_channel_index(m, req->channel_id);
      if (ci == SIZE_MAX || m->channels[ci].discord_type != CCORD_DISCORD_CHANNEL_GROUP_DM) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      }
      ccord_channel *ch = &m->channels[ci];
      if (req->has_group_dm_name) {
        char def_name[CCORD_NAME_MAX_BYTES + 1];
        const char *raw_name = req->name;
        if (raw_name[0] == '\0') {
          mock_default_group_dm_name(m, ch->recipient_ids, ch->recipient_count, def_name, sizeof def_name);
          raw_name = def_name;
        }
        const char *owned_name = mk_admin_owned(m, raw_name);
        if (!owned_name) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
        ch->name = owned_name;
      }
      if (req->has_group_dm_icon) {
        if (req->clear_group_dm_icon) {
          ch->icon_url = "";
        } else {
          char url[96];
          (void)snprintf(url, sizeof url, "https://cdn.discordapp.com/channel-icons/%llu/gdm_%zu.png",
                         (unsigned long long)ch->id, m->admin_count);
          const char *owned_url = mk_admin_owned(m, url);
          if (!owned_url) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
          ch->icon_url = owned_url;
        }
      }
      return mk_admin_emit_channel_op(m, ci, CCORD_ADMIN_EDIT_GROUP_DM, err);
    }
    case CCORD_ADMIN_SET_USER_NOTE: {
      if (req->user_id == CCORD_ID_NONE) {
        return mk_admin_reply_failed(m, req, CCORD_ERR_INVALID, "Invalid user", err);
      }
      const char *owned_note = mk_admin_owned(m, req->note);
      if (!owned_note) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
      if (req->note[0] == '\0') {
        for (size_t i = 0; i < m->nnotes; i++) {
          if (m->notes[i].user_id == req->user_id) {
            memmove(&m->notes[i], &m->notes[i + 1u], (m->nnotes - i - 1u) * sizeof m->notes[0]);
            m->nnotes--;
            m->snap.note_count = m->nnotes;
            break;
          }
        }
      } else {
        bool updated = false;
        for (size_t i = 0; i < m->nnotes; i++) {
          if (m->notes[i].user_id == req->user_id) {
            m->notes[i].note = owned_note;
            updated = true;
            break;
          }
        }
        if (!updated && m->nnotes < MOCK_NUM_USERS) {
          m->notes[m->nnotes++] = (ccord_user_note){.user_id = req->user_id, .note = owned_note};
          m->snap.note_count = m->nnotes;
        }
      }
      mock_item it = mk_make_item(CCORD_NET_USER_NOTE_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.user_id = req->user_id;
      it.ev.text = owned_note;
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_SET_CUSTOM_STATUS: {
      if (req->custom_status_clear) {
        m->users[0].custom_status_emoji[0] = '\0';
        m->users[0].custom_status_text[0] = '\0';
        m->users[0].custom_status_expires_ms = 0;
      } else {
        if (req->custom_status_emoji_id != CCORD_ID_NONE && req->custom_status_emoji[0] != '\0') {   // stored as name:id
          (void)snprintf(m->users[0].custom_status_emoji, sizeof m->users[0].custom_status_emoji, "%.40s:%llu",
                         req->custom_status_emoji, (unsigned long long)req->custom_status_emoji_id);
        } else {
          (void)snprintf(m->users[0].custom_status_emoji, sizeof m->users[0].custom_status_emoji, "%s",
                         req->custom_status_emoji);
        }
        (void)snprintf(m->users[0].custom_status_text, sizeof m->users[0].custom_status_text, "%s",
                       req->custom_status_text);
        m->users[0].custom_status_expires_ms = req->until_ms;
      }
      mock_item it = mk_make_item(CCORD_NET_CUSTOM_STATUS_UPDATE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.user_id = m->self_id;
      it.ev.custom_status_clear = req->custom_status_clear;
      if (!req->custom_status_clear) {
        (void)snprintf(it.ev.custom_status_emoji, sizeof it.ev.custom_status_emoji, "%s", m->users[0].custom_status_emoji);
        (void)snprintf(it.ev.custom_status_text, sizeof it.ev.custom_status_text, "%s", req->custom_status_text);
        it.ev.custom_status_expires_ms = req->until_ms;
      }
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_SET_NICK:
    case CCORD_ADMIN_TIMEOUT: {
      long mi = mk_admin_member_index(m, req->guild_id, req->user_id);
      if (mi < 0) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      if (req->op == CCORD_ADMIN_SET_NICK) {
        memset(m->members[mi].nick, 0, sizeof m->members[mi].nick);
        (void)snprintf(m->members[mi].nick, sizeof m->members[mi].nick, "%s", req->name);
      } else {
        m->members[mi].timeout_until_ms = req->until_ms;
      }
      ccord_status as = mock_audit_add(m, req->guild_id, 24, req->user_id, mock_user_name(m, req->user_id), req->reason,
                                       req->op == CCORD_ADMIN_SET_NICK ? "nickname changed"
                                       : req->until_ms                 ? "timed out"
                                                                       : "timeout removed");
      if (as != CCORD_OK) return as;
      return mk_admin_emit_member(m, (size_t)mi, err);
    }
    case CCORD_ADMIN_KICK:
    case CCORD_ADMIN_BAN: {
      long mi = mk_admin_member_index(m, req->guild_id, req->user_id);
      if (mi < 0) return mk_admin_reply_failed(m, req, CCORD_ERR_NOT_FOUND, gone, err);
      const char *who = mk_admin_owned(m, mock_user_name(m, req->user_id));
      mock_mrow *row = mock_mrow_find(m, req->guild_id, req->user_id);
      if (row) row->present = false;
      mock_sync_member_roles(m);
      memmove(&m->members[mi], &m->members[mi + 1], (m->nmembers - (size_t)mi - 1) * sizeof *m->members);
      m->nmembers--;
      m->snap.member_count = m->nmembers;
      if (req->op == CCORD_ADMIN_BAN && m->nbans < MOCK_BANS_MAX && who) {
        const char *why = mk_admin_owned(m, req->reason);
        if (why) m->bans[m->nbans++] = (mock_ban){req->guild_id, {.user_id = req->user_id, .name = who, .reason = why}};
      }
      ccord_status as = mock_audit_add(m, req->guild_id, req->op == CCORD_ADMIN_BAN ? 22 : 20, req->user_id, who,
                                       req->reason, "");
      if (as != CCORD_OK) return as;
      mock_item it = mk_make_item(CCORD_NET_MEMBER_REMOVE, mk_now_of(m) + m->cfg.latency_ms);
      it.ev.guild_id = req->guild_id;
      it.ev.user_id = req->user_id;
      return mk_push_item(m, &it, err);
    }
    case CCORD_ADMIN_NONE:
    case CCORD_ADMIN_CREATE_ROLE:
    case CCORD_ADMIN_EDIT_ROLE:
    case CCORD_ADMIN_DELETE_ROLE:
    case CCORD_ADMIN_REORDER_ROLES:
    case CCORD_ADMIN_ADD_MEMBER_ROLE:
    case CCORD_ADMIN_REMOVE_MEMBER_ROLE:
    case CCORD_ADMIN_UNBAN:
    case CCORD_ADMIN_CREATE_INVITE:
    case CCORD_ADMIN_REVOKE_INVITE:
    case CCORD_ADMIN_LIST_BANS:
    case CCORD_ADMIN_LIST_INVITES:
    case CCORD_ADMIN_LIST_AUDIT_LOG:   // served by mock_admin_server above
    case CCORD_ADMIN_REORDER_CHANNELS: // served by mock_admin_server above
    case CCORD_ADMIN_EDIT_GUILD:       // served by mock_admin_server above
    case CCORD_ADMIN_SET_INCIDENT_ACTIONS:
    case CCORD_ADMIN_SET_VANITY_URL:
    case CCORD_ADMIN_LEAVE_GUILD:
    case CCORD_ADMIN_DELETE_GUILD:
    case CCORD_ADMIN_TRANSFER_OWNERSHIP:
    case CCORD_ADMIN_LIST_EMOJIS:
    case CCORD_ADMIN_CREATE_EMOJI:
    case CCORD_ADMIN_EDIT_EMOJI:
    case CCORD_ADMIN_DELETE_EMOJI:
    case CCORD_ADMIN_LIST_STICKERS:
    case CCORD_ADMIN_CREATE_STICKER:
    case CCORD_ADMIN_EDIT_STICKER:
    case CCORD_ADMIN_DELETE_STICKER:
    case CCORD_ADMIN_LIST_SOUNDBOARD:
    case CCORD_ADMIN_CREATE_SOUNDBOARD:
    case CCORD_ADMIN_EDIT_SOUNDBOARD:
    case CCORD_ADMIN_DELETE_SOUNDBOARD:
    case CCORD_ADMIN_PREVIEW_PRUNE:
    case CCORD_ADMIN_EXECUTE_PRUNE:
    case CCORD_ADMIN_LIST_AUTOMOD_RULES:
    case CCORD_ADMIN_CREATE_AUTOMOD_RULE:
    case CCORD_ADMIN_EDIT_AUTOMOD_RULE:
    case CCORD_ADMIN_DELETE_AUTOMOD_RULE:
    case CCORD_ADMIN_LIST_WEBHOOKS:
    case CCORD_ADMIN_CREATE_WEBHOOK:
    case CCORD_ADMIN_EDIT_WEBHOOK:
    case CCORD_ADMIN_DELETE_WEBHOOK:
    case CCORD_ADMIN_FOLLOW_CHANNEL:
    case CCORD_ADMIN_LIST_APP_CMD_PERMS:
    case CCORD_ADMIN_GET_WELCOME_SCREEN:
    case CCORD_ADMIN_EDIT_WELCOME_SCREEN:
    case CCORD_ADMIN_GET_ONBOARDING:
    case CCORD_ADMIN_PUT_ONBOARDING:
    case CCORD_ADMIN_PUT_CHANNEL_OVERWRITE:
    case CCORD_ADMIN_DELETE_CHANNEL_OVERWRITE:
    case CCORD_ADMIN_LIST_CHANNEL_INVITES:
    case CCORD_ADMIN_CLONE_CHANNEL:
    case CCORD_ADMIN_SET_VOICE_MUTE:
    case CCORD_ADMIN_SET_VOICE_DEAF:
    case CCORD_ADMIN_MOVE_VOICE_MEMBER:
    case CCORD_ADMIN_SET_NOTIFY:       // served before the permission check
    case CCORD_ADMIN_SET_GUILD_FOLDERS:
    case CCORD_ADMIN_CREATE_GUILD:
    case CCORD_ADMIN_PREVIEW_INVITE:
    case CCORD_ADMIN_ACCEPT_INVITE:
    case CCORD_ADMIN_GET_MEMBER_VERIFICATION:      // served before the permission check
    case CCORD_ADMIN_SUBMIT_MEMBER_VERIFICATION:
    case CCORD_ADMIN_GET_GUILD_PROFILE:
    case CCORD_ADMIN_OP_COUNT: break;
    default: break;  // M5 ops are served by m5-session-mock
  }
  return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad admin request");
}

size_t ccord_mock_admin_count(const ccord_transport *t) { return t ? ((const mock *)t)->admin_count : 0; }

size_t ccord_mock_call_count(const ccord_transport *t, ccord_mock_call which)
{
  return t && which < CCORD_MOCK_CALL_COUNT ? ((const mock *)t)->calls[which] : 0;
}

uint32_t ccord_mock_last_member_chunks(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_member_chunks : 0;
}

const ccord_admin_request *ccord_mock_last_admin(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_admin : nullptr;
}

const char *ccord_mock_rich_activities(const ccord_transport *t, size_t *len)
{
  const mock *m = (const mock *)t;
  if (len) *len = m ? m->rich_len : 0;
  return m ? m->rich_json : nullptr;
}


