// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "mock_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void mk_item_release(mock_item *it)
{
  ccord_free(it->msgs);
  ccord_free(it->vstate);
  ccord_free(it->stream);
  ccord_free(it->sig);
  ccord_free(it->chan);
  ccord_free(it->media_data);
  ccord_free(it->profile);
  ccord_free(it->fposts);
  ccord_free(it->draft);
  ccord_free(it->member);
  ccord_free(it->guild);
  ccord_free(it->roles_own);
  ccord_free(it->bans_own);
  ccord_free(it->invites_own);
  ccord_free(it->audit_own);
  ccord_free(it->notify_own);
  ccord_free(it->modal_own);
  ccord_free(it->slash_own);
  ccord_free(it->choices_own);
  ccord_free(it->users_own);
  ccord_free(it->folder_items_own);
  ccord_free(it->reorder_ids_own);
  ccord_free(it->reorder_pos_own);
  ccord_free(it->reorder_par_own);
  ccord_free(it->reorder_hpar_own);
  ccord_free(it->invite_preview_own);
  ccord_free(it->discovery_own);
  ccord_free(it->discovery_query_own);
  ccord_free(it->emojis_own);
  ccord_free(it->stickers_own);
  ccord_free(it->soundboard_own);
  ccord_free(it->automod_own);
  ccord_free(it->webhooks_own);
  ccord_free(it->app_perms_own);
  ccord_free(it->welcome_own);
  ccord_free(it->onboarding_own);
  ccord_free(it->forum_metas_own);
  ccord_free(it->overwrites_own);
  ccord_free(it->overwrite_items_own);
  ccord_free(it->call_own);
  ccord_free(it->activity_apps_own);
  ccord_free(it->stage_own);
  ccord_free(it->vserver_own);
  ccord_free(it->voice_status_own);
  ccord_free(it->settings_own);
  ccord_free(it->auth_sessions_own);
  ccord_free(it->connections_own);
  ccord_free(it->events_own);
  ccord_free(it->mv_own);
  ccord_free(it->raw_proto_own);
}


size_t mk_qidx(const mock *m, size_t logical)
{
  return (m->qhead + logical) % m->capq;
}

static void free_queue(mock *m)
{
  for (size_t i = 0; i < m->nq; i++) mk_item_release(&m->queue[mk_qidx(m, i)]);
  m->qhead = 0;
  m->nq = 0;
}

static bool mock_queue_linearize(mock *m)
{
  if (m->qhead == 0 || m->nq == 0) {
    m->qhead = 0;
    return true;
  }
  mock_item *tmp = ccord_alloc_array(m->nq, sizeof *tmp);
  if (!tmp) return false;
  size_t first = m->capq - m->qhead;
  if (first > m->nq) first = m->nq;
  memcpy(tmp, m->queue + m->qhead, first * sizeof *tmp);
  if (m->nq > first) memcpy(tmp + first, m->queue, (m->nq - first) * sizeof *tmp);
  memcpy(m->queue, tmp, m->nq * sizeof *tmp);
  ccord_free(tmp);
  m->qhead = 0;
  return true;
}

static void dataset_free(mock *m)
{
  for (size_t ci = 0; ci < m->nchannels; ci++) {
    for (size_t k = 0; m->chmsgs && k < m->chn[ci]; k++) {
      mk_free_content(&m->chmsgs[ci][k].content);
      mk_free_rich(&m->chmsgs[ci][k].rich);
    }
  }
  for (size_t ci = 0; m->chmsgs && ci < m->nchannels; ci++) ccord_free(m->chmsgs[ci]);
  ccord_free(m->chmsgs);
  ccord_free(m->chn);
  ccord_free(m->chcap);
  ccord_free(m->channels);
  ccord_free(m->members);
  ccord_free(m->forum_posts);
  for (size_t i = 0; i < m->ngarbage; i++) mk_free_content(&m->garbage[i]);
  ccord_free(m->garbage);
  for (size_t i = 0; i < m->nrgarbage; i++) ccord_message_rich_free(m->rgarbage[i]);
  ccord_free(m->rgarbage);
  m->rgarbage = nullptr;
  m->nrgarbage = m->caprgarbage = 0;
  ccord_free(m->settings_proto);
  ccord_free(m->status1);
  for (size_t i = 0; i < m->nstatus1_patches; i++) {
    ccord_free(m->status1_patches[i].raw);
    ccord_free(m->status1_patches[i].body);
  }
  ccord_free(m->status1_patches);
  ccord_arena_free(&m->automod_arena);
}

// ---- queue ----

ccord_status mk_push_item(mock *m, const mock_item *item, ccord_error *err)
{
  if (m->nq == m->capq) {
    size_t ncap = m->capq ? m->capq * 2u : 16u;
    mock_item *g = ccord_alloc_array(ncap, sizeof *g);
    if (!g) {
      mock_item drop = *item;
      mk_item_release(&drop);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    if (m->nq > 0) {
      size_t first = m->capq - m->qhead;
      if (first > m->nq) first = m->nq;
      memcpy(g, m->queue + m->qhead, first * sizeof *g);
      if (m->nq > first) memcpy(g + first, m->queue, (m->nq - first) * sizeof *g);
    }
    ccord_free(m->queue);
    m->queue = g;
    m->qhead = 0;
    m->capq = ncap;
  }
  m->queue[(m->qhead + m->nq) % m->capq] = *item;
  m->nq++;
  return CCORD_OK;
}

mock_item mk_make_item(ccord_net_event_type type, uint64_t due)
{
  mock_item it = {.ev = {.type = type}, .msgs = nullptr, .due = due};
  return it;
}

size_t mk_channel_index(const mock *m, ccord_id channel_id, bool text_only, ccord_status *st, ccord_error *err)
{
  for (size_t i = 0; i < m->nchannels; i++) {
    if (m->channels[i].id == channel_id) {
      // "text_only" means "has a chat": text channels plus a voice/stage channel's own chat.
      ccord_channel_kind k = m->channels[i].kind;
      if (text_only && k != CCORD_CHANNEL_TEXT && k != CCORD_CHANNEL_VOICE && k != CCORD_CHANNEL_STAGE) {
        *st = ccord_error_set(err, CCORD_ERR_INVALID, "mock: channel has no chat");
        return SIZE_MAX;
      }
      *st = CCORD_OK;
      return i;
    }
  }
  *st = ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown channel");
  return SIZE_MAX;
}

// Appends a message to the dataset (content copied) and returns the stored struct. `rich` (may be null) is copied too; the
// dataset owns the copy and queued events share it.

int64_t mk_message_time(const mock *m)
{
  return (m->cfg.base_time_ms ? m->cfg.base_time_ms : MOCK_DEFAULT_BASE_MS) +
         (int64_t)(mk_now_of(m) - m->start_ms);
}

long mk_find_message(const mock *m, size_t ci, ccord_id message_id)
{
  for (size_t i = 0; i < m->chn[ci]; i++) {
    if (m->chmsgs[ci][i].id == message_id) return (long)i;
  }
  return -1;
}

bool mock_guild_known(const mock *m, ccord_id guild_id)
{
  for (size_t i = 0; i < m->nguilds; i++) if (m->guilds[i].id == guild_id) return true;
  return false;
}

long mock_voice_index(const mock *m, ccord_id guild_id, ccord_id user_id)
{
  for (size_t i = 0; i < m->nvoices; i++) {
    if (m->voices[i].guild_id == guild_id && m->voices[i].user_id == user_id) return (long)i;
  }
  return -1;
}

ccord_status mk_queue_activity_instance(mock *m, const ccord_activity_instance *inst, uint64_t due,
                                            ccord_error *err)
{
  mock_item it = mk_make_item(CCORD_NET_ACTIVITY_INSTANCE_UPDATE, due);
  it.ev.guild_id = inst->guild_id;
  it.ev.channel_id = inst->channel_id;
  it.ev.activity_instance = *inst;
  return mk_push_item(m, &it, err);
}


// ---- vtable ----

static const ccord_emoji k_seed_emojis[] = {
  {.id = 5001, .guild_id = 2001, .name = "ccord", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5002, .guild_id = 2001, .name = "shipit", .animated = false, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5003, .guild_id = 2001, .name = "valgrind", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5004, .guild_id = 2001, .name = "partyblob", .animated = true, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5005, .guild_id = 2001, .name = "c23", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5006, .guild_id = 2002, .name = "luajit", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5007, .guild_id = 2002, .name = "moon", .animated = false, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5008, .guild_id = 2002, .name = "coroutine", .animated = true, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5009, .guild_id = 2003, .name = "pixelheart", .animated = false, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 5010, .guild_id = 2003, .name = "sprout", .animated = false, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 5011, .guild_id = 2003, .name = "starlight", .animated = true, .available = true, .user_id = 1002, .user_name = "Ada"},
};

static const ccord_sticker k_seed_stickers[] = {
  {.id = 7001, .guild_id = 2001, .name = "WumpusWave", .description = "Friendly wave", .tags = "wave,hi", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 7002, .guild_id = 2001, .name = "CCordRocket", .description = "Blast off", .tags = "rocket,ship", .format = CCORD_STICKER_APNG, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 7003, .guild_id = 2001, .name = "ZeroLeak", .description = "0 bytes lost", .tags = "valgrind,clean", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 7004, .guild_id = 2001, .name = "LottieSparkle", .description = "Vector sparkle", .tags = "sparkle,lottie", .format = CCORD_STICKER_LOTTIE, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 7005, .guild_id = 2002, .name = "MoonCat", .description = "Lua cat", .tags = "cat,lua", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 7006, .guild_id = 2002, .name = "TableFlip", .description = "Nil index", .tags = "flip,table", .format = CCORD_STICKER_GIF, .available = true, .user_id = 1002, .user_name = "Ada"},
  {.id = 7007, .guild_id = 2003, .name = "PixelBloom", .description = "8-bit flower", .tags = "flower,pixel", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1001, .user_name = "You"},
  {.id = 7008, .guild_id = 2003, .name = "CozyTea", .description = "Warm cup", .tags = "tea,cozy", .format = CCORD_STICKER_PNG, .available = true, .user_id = 1002, .user_name = "Ada"},
};

static const ccord_soundboard_sound k_seed_soundboard[] = {
  {.id = 6501, .guild_id = 2001, .name = "airhorn", .volume_pct = 85, .emoji_name = "\xf0\x9f\x93\xa2", .user_id = 1001, .user_name = "You", .available = true},
  {.id = 6502, .guild_id = 2001, .name = "rimshot", .volume_pct = 100, .emoji_name = "\xf0\x9f\xa5\x81", .user_id = 1002, .user_name = "Ada", .available = true},
  {.id = 6503, .guild_id = 2001, .name = "compile_ok", .volume_pct = 75, .emoji_id = 5001, .emoji_name = "ccord", .user_id = 1001, .user_name = "You", .available = true},
  {.id = 6504, .guild_id = 2002, .name = "meow", .volume_pct = 90, .emoji_name = "\xf0\x9f\x90\xb1", .user_id = 1002, .user_name = "Ada", .available = true},
  {.id = 6505, .guild_id = 2003, .name = "chime", .volume_pct = 80, .emoji_name = "\xe2\x9c\xa8", .user_id = 1001, .user_name = "You", .available = true},
};

// Queues CONNECTED and the full SNAPSHOT (plus the gateway dispatches that follow it), all due at `due`. Shared by the
// first connect and by a fresh IDENTIFY after the link model lost the session.
static ccord_status mock_queue_ready(mock *m, uint64_t due, ccord_error *err)
{
  mock_item st = mk_make_item(CCORD_NET_STATE, due);
  st.ev.state = CCORD_CONN_CONNECTED;
  ccord_status s = mk_push_item(m, &st, err);
  if (s != CCORD_OK) return s;
  mock_item snap = mk_make_item(CCORD_NET_SNAPSHOT, due);
  snap.ev.snapshot = &m->snap;
  if (m->nemojis > 0) {
    snap.emojis_own = ccord_alloc_array(m->nemojis, sizeof *snap.emojis_own);
    if (!snap.emojis_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    for (size_t i = 0; i < m->nemojis; i++) {
      snap.emojis_own[i] = m->emojis[i];
      snap.emojis_own[i].user_id = CCORD_ID_NONE;
      snap.emojis_own[i].user_name = "";
    }
    snap.ev.emojis = snap.emojis_own;
    snap.ev.emoji_count = m->nemojis;
  }
  if (m->nstickers > 0) {
    snap.stickers_own = ccord_alloc_array(m->nstickers, sizeof *snap.stickers_own);
    if (!snap.stickers_own) {
      mk_item_release(&snap);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    for (size_t i = 0; i < m->nstickers; i++) {
      snap.stickers_own[i] = m->stickers[i];
      snap.stickers_own[i].user_id = CCORD_ID_NONE;
      snap.stickers_own[i].user_name = "";
    }
    snap.ev.stickers = snap.stickers_own;
    snap.ev.sticker_count = m->nstickers;
  }
  if (m->nsoundboard > 0) {
    snap.soundboard_own = ccord_alloc_array(m->nsoundboard, sizeof *snap.soundboard_own);
    if (!snap.soundboard_own) {
      mk_item_release(&snap);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    }
    for (size_t i = 0; i < m->nsoundboard; i++) {
      snap.soundboard_own[i] = m->soundboard[i];
      snap.soundboard_own[i].user_id = CCORD_ID_NONE;
      snap.soundboard_own[i].user_name = "";
    }
    snap.ev.soundboard = snap.soundboard_own;
    snap.ev.soundboard_count = m->nsoundboard;
  }
  snap.ev.forum_metas = m->forum_metas;
  snap.ev.forum_meta_count = m->nforum_metas;
  snap.ev.forum_posts = m->forum_posts;
  snap.ev.forum_post_count = m->nforum_posts;
  snap.ev.roles = m->roles;
  snap.ev.role_count = m->nroles;
  snap.ev.member_roles = m->member_roles;
  snap.ev.member_role_count = m->nmember_roles;
  snap.ev.overwrites = m->overwrites;
  snap.ev.overwrite_count = m->noverwrites;
  snap.ev.notify = m->notify;
  snap.ev.notify_count = m->nnotify;
  s = mk_push_item(m, &snap, err);
  if (s != CCORD_OK) return s;
  // Stage instances and scheduled events are gateway dispatches, not snapshot rows: they follow the snapshot.
  for (size_t i = 0; i < m->nstages; i++) {
    s = mk_queue_stage_instance(m, &m->stages[i], false, due, err);
    if (s != CCORD_OK) return s;
  }
  for (size_t gi = 0; gi < m->nguilds; gi++) {
    ccord_id gid = m->guilds[gi].id;
    size_t ev_n = 0;
    for (size_t ei = 0; ei < m->nevents; ei++) {
      if (m->events[ei].guild_id == gid) ev_n++;
    }
    if (ev_n == 0) continue;
    mock_item eit = mk_make_item(CCORD_NET_SCHEDULED_EVENT_LIST, due);
    eit.events_own = ccord_alloc_array(ev_n, sizeof *eit.events_own);
    if (!eit.events_own) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    size_t k = 0;
    for (size_t ei = 0; ei < m->nevents; ei++) {
      if (m->events[ei].guild_id == gid) eit.events_own[k++] = m->events[ei];
    }
    eit.ev.guild_id = gid;
    eit.ev.scheduled_events = eit.events_own;
    eit.ev.scheduled_event_count = ev_n;
    s = mk_push_item(m, &eit, err);
    if (s != CCORD_OK) return s;
  }
  // Running activity instances survive a reconnect: Discord re-sends them with the guild's state.
  for (size_t i = 0; i < m->nactivities; i++) {
    s = mk_queue_activity_instance(m, &m->activities[i], due, err);
    if (s != CCORD_OK) return s;
  }
  return CCORD_OK;
}

static void mock_link_on_connect(mock *m, uint64_t now);

static ccord_status mock_connect(ccord_transport *t, ccord_error *err)
{
  mock *m = (mock *)t;
  if (m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: already connected");
  m->connected = true;
  uint64_t now = mk_now_of(m);
  m->next_ambient = m->cfg.ambient_interval_ms ? now + m->cfg.ambient_interval_ms : 0;
  mock_link_on_connect(m, now);
  mock_item st = mk_make_item(CCORD_NET_STATE, now);
  st.ev.state = CCORD_CONN_CONNECTING;
  ccord_status s = mk_push_item(m, &st, err);
  if (s != CCORD_OK) return s;
  return mock_queue_ready(m, now + m->cfg.latency_ms, err);
}

static void mock_disconnect(ccord_transport *t)
{
  mock *m = (mock *)t;
  if (!m->connected) return;
  m->connected = false;
  m->link.state = MOCK_LINK_UP;
  m->link.dead_silent = false;
  mock_stream_media_clear(m);
  mock_cam_clear(m);
  m->ninflight_uploads = 0;
  free_queue(m);
  mock_item st = mk_make_item(CCORD_NET_STATE, mk_now_of(m));
  st.ev.state = CCORD_CONN_DISCONNECTED;
  mk_push_item(m, &st, nullptr);
}


static ccord_status mock_select_guild(ccord_transport *t, ccord_id guild_id, ccord_error *err)
{
  mock *m = (mock *)t;
  if (!m->connected) return ccord_error_set(err, CCORD_ERR_INVALID, "mock: not connected");
  if (guild_id == CCORD_DM_GUILD_ID) return CCORD_OK;
  for (size_t i = 0; i < m->nguilds; i++) {
    if (m->guilds[i].id == guild_id) return CCORD_OK;
  }
  return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown guild");
}

static ccord_status mock_select_channel(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, ccord_error *err)
{
  (void)channel_id;
  return mock_select_guild(t, guild_id, err);
}

static ccord_status mock_request_members(ccord_transport *t, ccord_id guild_id, ccord_id channel_id, uint32_t chunks,
                                         ccord_error *err)
{
  mock *m = (mock *)t;
  (void)channel_id;
  ccord_error_clear(err);
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: member list needs a real guild");
  }
  m->calls[CCORD_MOCK_CALL_MEMBERS]++;
  m->last_member_chunks = chunks;
  return CCORD_OK;
}


// ---- Gateway link model ----

static uint64_t mock_link_ttl(const mock *m)
{
  return m->link.ttl_set ? m->link.ttl_ms : MOCK_LINK_TTL_DEFAULT_MS;
}

// A fresh connection: heartbeat and uptime start now, the session is valid, and no earlier wake or attempt counts.
static void mock_link_on_connect(mock *m, uint64_t now)
{
  mock_link *l = &m->link;
  l->state = MOCK_LINK_UP;
  l->dead_silent = false;
  l->session_expired = false;
  l->attempt_resume = false;
  ccord_gwpace_reset(&l->pace);
  l->pace.wake_last_ms = 0;
  l->pace.attempt_ms = 0;
  ccord_gwpace_hello(&l->pace, now, MOCK_LINK_HB_INTERVAL_MS);
  ccord_gwpace_connected(&l->pace, now);
}

static void mock_link_push_state(mock *m, ccord_connection_state state, uint64_t due)
{
  mock_item st = mk_make_item(CCORD_NET_STATE, due);
  st.ev.state = state;
  (void)mk_push_item(m, &st, nullptr);   // out of memory only loses a state notification
}

// The socket is gone (closed, errored or declared a zombie): the app sees CONNECTING and a reconnect is up to the pace.
static void mock_link_lose(mock *m, uint64_t now)
{
  mock_link *l = &m->link;
  l->state = MOCK_LINK_DOWN;
  l->dead_silent = false;
  ccord_gwpace_disconnected(&l->pace);
  mock_link_push_state(m, CCORD_CONN_CONNECTING, now);
}

// Moves the last `added` queue items to the front, so a reconnect's CONNECTED / SNAPSHOT come before events that were
// held back while the link was down (a real RESUME would replay those after RESUMED).
static void mock_queue_front(mock *m, size_t first)
{
  size_t added = m->nq - first;
  if (first == 0 || added == 0) return;
  if (!mock_queue_linearize(m)) return;   // order is cosmetic; keep the items
  mock_item *tmp = ccord_alloc_array(added, sizeof *tmp);
  if (!tmp) return;   // order is cosmetic; keep the items
  // A CONNECTING queued by the loss that this reconnect repairs (same instant when latency is 0) stays ahead of it, or the
  // session would end up CONNECTING after the reconnect finished.
  size_t at = 0;
  for (size_t i = 0; i < first; i++) {
    if (m->queue[i].ev.type == CCORD_NET_STATE && m->queue[i].ev.state == CCORD_CONN_CONNECTING) at = i + 1;
  }
  memcpy(tmp, m->queue + first, added * sizeof *tmp);
  memmove(m->queue + at + added, m->queue + at, (first - at) * sizeof *m->queue);
  memcpy(m->queue + at, tmp, added * sizeof *tmp);
  ccord_free(tmp);
}

static void mock_link_up(mock *m, uint64_t now)
{
  mock_link *l = &m->link;
  l->state = MOCK_LINK_UP;
  ccord_gwpace_connected(&l->pace, now);
  ccord_gwpace_hello(&l->pace, now, MOCK_LINK_HB_INTERVAL_MS);
  size_t first = m->nq;
  if (l->attempt_resume) {
    mock_link_push_state(m, CCORD_CONN_CONNECTED, now);
  } else {
    l->session_expired = false;
    (void)mock_queue_ready(m, now, nullptr);
  }
  mock_queue_front(m, first);
}

// The planned reconnect is due: open the socket (RESUME or IDENTIFY), or fail like a refused open.
static void mock_link_attempt(mock *m, uint64_t now, bool resume)
{
  mock_link *l = &m->link;
  ccord_gwpace_reconnect_started(&l->pace);
  l->stats.attempts++;
  if (l->network_down) {
    (void)ccord_gwpace_plan_reconnect(&l->pace, now, CCORD_GWPACE_NETWORK, !l->session_expired);
    return;
  }
  if (resume && !l->session_expired) {
    if (now >= l->lost_ms && now - l->lost_ms > mock_link_ttl(m)) {
      // op 9 d:false: the session is gone; wait 1-5 s, then IDENTIFY.
      l->session_expired = true;
      (void)ccord_gwpace_plan_reconnect(&l->pace, now, CCORD_GWPACE_INVALID_SESSION, false);
      return;
    }
    l->attempt_resume = true;
    l->stats.resumes++;
  } else {
    l->attempt_resume = false;
    l->stats.identifies++;
    ccord_gwpace_identify_sent(&l->pace, now);
  }
  l->state = MOCK_LINK_ATTEMPT;
  l->up_ms = now + m->cfg.latency_ms;
}

// Poll-side upkeep, like ccord_gateway_poll: heartbeat, zombie detection, due reconnects, a finished socket open.
static void mock_link_tick(mock *m, uint64_t now)
{
  mock_link *l = &m->link;
  if (l->state == MOCK_LINK_UP) {
    ccord_gwpace_tick(&l->pace, now);
    ccord_gwpace_beat beat = ccord_gwpace_heartbeat_due(&l->pace, now);
    if (beat == CCORD_GWPACE_BEAT_SEND) {
      ccord_gwpace_heartbeat_sent(&l->pace, now);
      if (!l->dead_silent) (void)ccord_gwpace_heartbeat_ack(&l->pace, now);
    } else if (beat == CCORD_GWPACE_BEAT_ZOMBIE) {
      mock_link_lose(m, now);
      (void)ccord_gwpace_plan_reconnect(&l->pace, now, CCORD_GWPACE_ZOMBIE, !l->session_expired);
    }
  }
  bool resume = false;
  if (l->state == MOCK_LINK_DOWN && ccord_gwpace_reconnect_due(&l->pace, now, &resume)) mock_link_attempt(m, now, resume);
  if (l->state == MOCK_LINK_ATTEMPT && now >= l->up_ms) mock_link_up(m, now);
}

static ccord_status mock_network_event(ccord_transport *t, ccord_link_event kind, ccord_error *err)
{
  mock *m = (mock *)t;
  ccord_error_clear(err);
  if (kind != CCORD_LINK_RESUMED && kind != CCORD_LINK_CHANGED) return CCORD_OK;
  mock_link *l = &m->link;
  uint64_t now = mk_now_of(m);
  l->stats.wakes_seen++;
  ccord_gwpace_wake_verdict v = CCORD_GWPACE_WAKE_IGNORED;
  if (m->connected) v = ccord_gwpace_plan_wake(&l->pace, now, kind == CCORD_LINK_CHANGED, !l->session_expired);
  l->stats.last_verdict = v;
  if (v != CCORD_GWPACE_WAKE_ACCEPTED) return CCORD_OK;
  l->stats.wakes_accepted++;
  // Close the socket (dead or not) and let the next poll reconnect; the plan is already due.
  if (l->state == MOCK_LINK_UP) {
    if (!l->dead_silent) l->lost_ms = now;
    mock_link_lose(m, now);
  } else {
    l->state = MOCK_LINK_DOWN;
  }
  return CCORD_OK;
}

void ccord_mock_link_stats_get(const ccord_transport *t, ccord_mock_link_stats *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!t) return;
  const mock *m = (const mock *)t;
  *out = m->link.stats;
  out->dead_silent = m->link.dead_silent;
  if (ccord_gwpace_reconnect_pending(&m->link.pace)) {
    uint64_t now = mk_now_of(m);
    uint64_t at = m->link.pace.reconnect_at_ms;
    out->next_attempt_in_ms = at > now ? at - now : 0;
  }
}

void ccord_mock_drop_gateway(ccord_transport *t, bool silent)
{
  if (!t) return;
  mock *m = (mock *)t;
  if (!m->connected) return;
  mock_link *l = &m->link;
  uint64_t now = mk_now_of(m);
  if (l->state != MOCK_LINK_UP) return;
  if (!l->dead_silent) l->lost_ms = now;
  if (silent) {
    l->dead_silent = true;
    return;
  }
  mock_link_lose(m, now);
  (void)ccord_gwpace_plan_reconnect(&l->pace, now, CCORD_GWPACE_NETWORK, !l->session_expired);
}

void ccord_mock_set_network(ccord_transport *t, bool up)
{
  if (t) ((mock *)t)->link.network_down = !up;
}

void ccord_mock_set_session_ttl(ccord_transport *t, uint64_t ms)
{
  if (!t) return;
  mock *m = (mock *)t;
  m->link.ttl_set = true;
  m->link.ttl_ms = ms;
}

static bool mock_poll(ccord_transport *t, ccord_net_event *out)
{
  mock *m = (mock *)t;
  // Pointers handed out by the previous poll end here.
  if (m->have_current) {
    mk_item_release(&m->current);
    m->have_current = false;
    memset(&m->current, 0, sizeof m->current);
  }
  uint64_t now = mk_now_of(m);
  if (m->connected) mock_link_tick(m, now);
  if (m->connected && m->cfg.ambient_interval_ms && now >= m->next_ambient && m->next_ambient) {
    m->next_ambient = now + m->cfg.ambient_interval_ms;
    ccord_status cst;
    size_t ci = mk_channel_index(m, MOCK_GENERAL_ID, true, &cst, nullptr);
    if (ci != SIZE_MAX) {
      char text[64];
      int n = snprintf(text, sizeof text, "ambient message %llu", (unsigned long long)m->next_msg_id);
      ccord_status st;
      ccord_message msg = mk_store_new(m, ci, m->users[10].id, text, (size_t)n, nullptr, &st);
      if (st == CCORD_OK) (void)mk_queue_message_create(m, msg, now, nullptr);
    }
  }
  // Link down: only the mock's own state events get through; a silently dead socket delivers nothing at all.
  size_t pick = 0;
  if (m->connected && (m->link.state != MOCK_LINK_UP || m->link.dead_silent)) {
    pick = SIZE_MAX;
    if (!m->link.dead_silent) {
      for (size_t i = 0; i < m->nq; i++) {
        size_t si = mk_qidx(m, i);
        if (m->queue[si].ev.type == CCORD_NET_STATE && m->queue[si].due <= now) {
          pick = i;
          break;
        }
      }
    }
  }
  if (m->nq == 0 || pick == SIZE_MAX || m->queue[mk_qidx(m, pick)].due > now) return false;

  m->current = m->queue[mk_qidx(m, pick)];
  m->have_current = true;
  if (pick == 0) {
    m->qhead = (m->qhead + 1u) % m->capq;
    m->nq--;
    if (m->nq == 0) m->qhead = 0;
  } else {
    for (size_t i = pick; i + 1u < m->nq; i++) {
      m->queue[mk_qidx(m, i)] = m->queue[mk_qidx(m, i + 1u)];
    }
    m->nq--;
  }
  if (m->current.ev.type == CCORD_NET_MESSAGE_CREATE && m->current.ev.message_count > 0 && m->current.ev.messages) {
    ccord_id mid = m->current.ev.messages[0].id;
    for (size_t i = 0; i < m->ninflight_uploads; i++) {
      if (m->inflight_uploads[i].message_id == mid) {
        memmove(&m->inflight_uploads[i], &m->inflight_uploads[i + 1u],
                (m->ninflight_uploads - i - 1u) * sizeof m->inflight_uploads[0]);
        m->ninflight_uploads--;
        break;
      }
    }
  }
  *out = m->current.ev;
  return true;
}

ccord_id ccord_mock_last_ring_channel(const ccord_transport *t)
{
  return t ? ((const mock *)t)->last_ring_channel : CCORD_ID_NONE;
}

void ccord_mock_set_ring_refusal(ccord_transport *t, const char *reason)
{
  if (!t) return;
  mock *m = (mock *)t;
  (void)snprintf(m->ring_refusal, sizeof m->ring_refusal, "%s", reason ? reason : "");
}

void ccord_mock_set_late_poll_failure(ccord_transport *t, const char *reason)
{
  if (!t) return;
  mock *m = (mock *)t;
  (void)snprintf(m->late_poll_fail, sizeof m->late_poll_fail, "%s", reason ? reason : "");
}

// Whole outage computed now with the real policy (voice_reconnect.c) on the mock clock; see mock_transport.h.

void ccord_mock_set_refuse_writes(ccord_transport *t, bool refuse)
{
  if (t) ((mock *)t)->cfg.refuse_writes = refuse;
}

ccord_status ccord_mock_add_guild_member(ccord_transport *t, ccord_id guild_id, ccord_id user_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!t || guild_id == CCORD_ID_NONE || user_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "mock: bad add member args");
  }
  mock *m = (mock *)t;
  if (mock_guild_index(m, guild_id) == SIZE_MAX) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "mock: unknown guild");
  }
  long existing = mk_admin_member_index(m, guild_id, user_id);
  size_t mi = 0;
  if (existing >= 0) {
    mi = (size_t)existing;
  } else {
    ccord_member *nm = ccord_realloc_array(m->members, m->nmembers + 1u, sizeof *nm);
    if (!nm) return ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    m->members = nm;
    mi = m->nmembers++;
    m->members[mi] = (ccord_member){
      .guild_id = guild_id, .user_id = user_id, .role_name = "Members", .role_color = 0u, .role_rank = 2u,
    };
    m->snap.members = m->members;
    m->snap.member_count = m->nmembers;
    for (size_t i = 0; i < sizeof m->mrows / sizeof m->mrows[0]; i++) {
      if (!m->mrows[i].present) {
        m->mrows[i] = (mock_mrow){.guild_id = guild_id, .user_id = user_id, .present = true};
        mock_sync_member_roles(m);
        break;
      }
    }
  }
  return mk_admin_emit_member(m, mi, err);
}


static void mock_destroy(ccord_transport *t)
{
  mock *m = (mock *)t;
  free_queue(m);
  ccord_free(m->queue);
  if (m->have_current) mk_item_release(&m->current);
  mk_clear_last_interaction_values(m);
  mock_stream_media_clear(m);
  mock_cam_clear(m);
  ccord_free(m->last_admin);
  mock_report_free(m->last_report);
  ccord_free(m->rich_json);
  mk_upload_cache_free(m);
  dataset_free(m);
  ccord_free(m);
}

static const ccord_transport_vtable mock_vtable = {
  .name = "mock",
  .connect = mock_connect,
  .disconnect = mock_disconnect,
  .request_history = mock_request_history,
  .send_message = mock_send_message,
  .send_message_with_nonce = mock_send_message_with_nonce,
  .poll = mock_poll,
  .destroy = mock_destroy,
  .edit_message = mock_edit_message,
  .delete_message = mock_delete_message,
  .send_typing = mock_send_typing,
  .toggle_reaction = mock_toggle_reaction,
  .ack_message = mock_ack_message,
  .select_guild = mock_select_guild,
  .join_voice = mock_join_voice,
  .leave_voice = mock_leave_voice,
  .update_voice_state = mock_update_voice_state,
  .start_stream = mock_start_stream,
  .stop_stream = mock_stop_stream,
  .send_stream_frame = mock_send_stream_frame,
  .poll_stream_frame = mock_poll_stream_frame,
  .camera_senders = mock_camera_senders,
  .poll_camera_frame = mock_poll_camera_frame,
  .send_stream_pcm = mock_send_stream_pcm,
  .poll_stream_pcm = mock_poll_stream_pcm,
  .send_voice_video = mock_send_voice_video,
  .consume_keyframe_request = mock_consume_keyframe_request,
  .watch_stream = mock_watch_stream,
  .stop_watching = mock_stop_watching,
  .set_stream_paused = mock_set_stream_paused,
  .send_reply = mock_send_reply,
  .toggle_pin = mock_toggle_pin,
  .request_history_before = mock_request_history_before,
  .request_pins = mock_request_pins,
  .set_presence = mock_set_presence,
  .admin_action = mock_admin_action,
  .set_voice_camera = mock_set_voice_camera,
  .send_message_with_file = mock_send_message_with_file,
  .fetch_media = mock_fetch_media,
  .fetch_media_range = mock_fetch_media_range,
  .search_messages = mock_search_messages,
  .open_dm = mock_open_dm,
  .push_voice_pcm = mock_push_voice_pcm,
  .pull_voice_pcm = mock_pull_voice_pcm,
  .set_relationship = mock_set_relationship,
  .request_profile = mock_request_profile,
  .select_channel = mock_select_channel,
  .request_members = mock_request_members,
  .request_expression_data = mock_request_expression_data,
  .send_sticker = mock_send_sticker,
  .request_forum_posts = mock_request_forum_posts,
  .create_forum_post = mock_create_forum_post,
  .vote_poll = mock_vote_poll,
  .send_interaction = mock_send_interaction,
  .send_reply_ex = mock_send_reply_ex,
  .send_message_with_files = mock_send_message_with_files,
  .create_poll = mock_create_poll,
  .request_slash_commands = mock_request_slash_commands,
  .search_messages_ex = mock_search_messages_ex,
  .request_pins_before = mock_request_pins_before,
  .request_reaction_users = mock_request_reaction_users,
  .request_discoverable_guilds = mock_request_discoverable_guilds,
  .request_profile_ex = mock_request_profile_ex,
  .set_presence_ex = mock_set_presence_ex,
  .call_action = mock_call_action,
  .play_soundboard = mock_play_soundboard,
  .set_user_volume = mock_set_user_volume,
  .set_voice_config = mock_set_voice_config,
  .send_tts_message = mock_send_tts_message,
  .forward_message = mock_forward_message,
  .crosspost_message = mock_crosspost_message,
  .mark_unread = mock_mark_unread,
  .patch_settings_proto = mock_patch_settings_proto,
  .set_status_proto = mock_set_status_proto,
  .set_rich_activities = mock_set_rich_activities,
  .request_report_menu = mock_request_report_menu,
  .submit_report = mock_submit_report,
  .network_event = mock_network_event,
  .active_uploads = mock_active_uploads,
  .resolve_video = mock_resolve_video,
};

ccord_transport *ccord_mock_transport_create(const ccord_mock_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  mock *m = ccord_calloc(1, sizeof *m);
  if (!m) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "mock: out of memory");
    return nullptr;
  }
  m->base.vt = &mock_vtable;
  if (cfg) {
    m->cfg = *cfg;
  }
  m->cfg.seed = cfg && cfg->seed ? cfg->seed : MOCK_DEFAULT_SEED;
  m->cfg.big_channel_messages = cfg && cfg->big_channel_messages ? cfg->big_channel_messages : MOCK_DEFAULT_BIG;
  m->cfg.base_time_ms = cfg && cfg->base_time_ms ? cfg->base_time_ms : MOCK_DEFAULT_BASE_MS;
  m->rng = m->cfg.seed;
  if (mk_build_dataset(m, err) != CCORD_OK) {
    mock_destroy(&m->base);
    return nullptr;
  }
  m->nemojis = sizeof k_seed_emojis / sizeof k_seed_emojis[0];
  memcpy(m->emojis, k_seed_emojis, sizeof k_seed_emojis);
  m->nstickers = sizeof k_seed_stickers / sizeof k_seed_stickers[0];
  memcpy(m->stickers, k_seed_stickers, sizeof k_seed_stickers);
  m->nsoundboard = sizeof k_seed_soundboard / sizeof k_seed_soundboard[0];
  memcpy(m->soundboard, k_seed_soundboard, sizeof k_seed_soundboard);
  m->start_ms = mk_now_of(m);
  ccord_gwpace_init(&m->link.pace, m->cfg.seed ^ 0x6C696E6Bull);
  return &m->base;
}

