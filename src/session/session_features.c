// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

session_slash_cache *sess_slash_cache_for(ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nslash_cache; i++) {
    if (s->slash_cache[i].guild_id == guild_id) return &s->slash_cache[i];
  }
  if (s->nslash_cache == s->capslash_cache) {
    size_t ncap = s->capslash_cache ? s->capslash_cache * 2u : 8u;
    session_slash_cache *g = ccord_realloc_array(s->slash_cache, ncap, sizeof *g);
    if (!g) return nullptr;
    s->slash_cache = g;
    s->capslash_cache = ncap;
  }
  session_slash_cache *ent = &s->slash_cache[s->nslash_cache++];
  memset(ent, 0, sizeof *ent);
  ent->guild_id = guild_id;
  return ent;
}

session_activity_shelf *sess_activity_shelf_for(ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nactivity_shelves; i++) {
    if (s->activity_shelves[i].guild_id == guild_id) {
      s->activity_shelves[i].last_use = ++s->cache_tick;
      return &s->activity_shelves[i];
    }
  }
  session_activity_shelf *ent;
  if (s->nactivity_shelves >= SESSION_ACTIVITY_SHELF_CACHE_MAX) {
    size_t victim = SIZE_MAX;
    for (size_t i = 0; i < s->nactivity_shelves; i++) {
      if (s->activity_shelves[i].loading) continue;
      if (victim == SIZE_MAX || s->activity_shelves[i].last_use < s->activity_shelves[victim].last_use) victim = i;
    }
    if (victim == SIZE_MAX) {
      victim = 0;
      for (size_t i = 1; i < s->nactivity_shelves; i++) {
        if (s->activity_shelves[i].last_use < s->activity_shelves[victim].last_use) victim = i;
      }
    }
    ent = &s->activity_shelves[victim];
  } else {
    if (s->nactivity_shelves == s->capactivity_shelves) {
      size_t ncap = s->capactivity_shelves ? s->capactivity_shelves * 2u : 4u;
      if (ncap > SESSION_ACTIVITY_SHELF_CACHE_MAX) ncap = SESSION_ACTIVITY_SHELF_CACHE_MAX;
      session_activity_shelf *g = ccord_realloc_array(s->activity_shelves, ncap, sizeof *g);
      if (!g) return nullptr;
      s->activity_shelves = g;
      s->capactivity_shelves = ncap;
    }
    ent = &s->activity_shelves[s->nactivity_shelves++];
  }
  memset(ent, 0, sizeof *ent);
  ent->guild_id = guild_id;
  ent->last_use = ++s->cache_tick;
  return ent;
}


static const char *dup_in_search_arena(ccord_arena *a, const char *str)
{
  if (!str || str[0] == '\0') return "";
  char *c = ccord_arena_strndup(a, str, strlen(str));
  return c ? c : "";
}

void sess_clear_search_results(ccord_session *s)
{
  for (size_t i = 0; s->search_rich && i < s->search_count; i++) ccord_message_rich_free(s->search_rich[i]);
  ccord_free(s->search_rich);
  s->search_rich = nullptr;
  ccord_free(s->search_results);
  s->search_results = nullptr;
  s->search_count = 0;
  s->search_total_results = 0;
  s->search_offset = 0;
  s->search_loading = false;
  ccord_arena_free(&s->search_arena);
}

void sess_clear_reaction_users(ccord_session *s)
{
  ccord_free(s->react_users);
  s->react_users = nullptr;
  s->react_user_count = 0;
  s->react_users_loading = false;
  s->react_users_channel_id = CCORD_ID_NONE;
  s->react_users_message_id = CCORD_ID_NONE;
  s->react_users_emoji[0] = '\0';
  ccord_arena_free(&s->react_users_arena);
}

void sess_store_reaction_users(ccord_session *s, const ccord_net_event *ev)
{
  ccord_free(s->react_users);
  s->react_users = nullptr;
  s->react_user_count = 0;
  ccord_arena_free(&s->react_users_arena);
  s->react_users_loading = false;
  s->react_users_channel_id = ev->channel_id;
  s->react_users_message_id = ev->message_id;
  if (ev->reaction_emoji && ev->reaction_emoji[0] != '\0') {
    size_t elen = strlen(ev->reaction_emoji);
    if (elen >= sizeof s->react_users_emoji) elen = sizeof s->react_users_emoji - 1u;
    memcpy(s->react_users_emoji, ev->reaction_emoji, elen);
    s->react_users_emoji[elen] = '\0';
  }
  if (!ev->users || ev->user_count == 0) return;
  ccord_arena_init(&s->react_users_arena, ev->user_count * 128u + 512u);
  s->react_users = ccord_calloc(ev->user_count, sizeof *s->react_users);
  if (!s->react_users) {
    ccord_arena_free(&s->react_users_arena);
    return;
  }
  for (size_t i = 0; i < ev->user_count; i++) {
    ccord_user u = ev->users[i];
    u.name = dup_in_search_arena(&s->react_users_arena, u.name);
    u.handle = dup_in_search_arena(&s->react_users_arena, u.handle);
    u.status_text = dup_in_search_arena(&s->react_users_arena, u.status_text);
    u.avatar_url = dup_in_search_arena(&s->react_users_arena, u.avatar_url);
    u.banner_url = dup_in_search_arena(&s->react_users_arena, u.banner_url);
    u.bio = dup_in_search_arena(&s->react_users_arena, u.bio);
    u.pronouns = dup_in_search_arena(&s->react_users_arena, u.pronouns);
    s->react_users[i] = u;
  }
  s->react_user_count = ev->user_count;
}


void sess_store_search_results(ccord_session *s, const ccord_net_event *ev)
{
  sess_clear_search_results(s);
  s->search_offset = ev->search_offset;
  s->search_total_results = ev->search_total_results >= ev->message_count
                            ? ev->search_total_results
                            : (uint32_t)ev->message_count;
  if (!ev->messages || ev->message_count == 0) return;
  ccord_arena_init(&s->search_arena, ev->message_count * 256u + 1024u);
  s->search_results = ccord_calloc(ev->message_count, sizeof *s->search_results);
  s->search_rich = ccord_calloc(ev->message_count, sizeof *s->search_rich);
  if (!s->search_results || !s->search_rich) {
    ccord_free(s->search_results);
    ccord_free(s->search_rich);
    s->search_results = nullptr;
    s->search_rich = nullptr;
    ccord_arena_free(&s->search_arena);
    return;
  }
  for (size_t i = 0; i < ev->message_count; i++) {
    ccord_message m = ev->messages[i];
    m.content = dup_in_search_arena(&s->search_arena, m.content);
    // The event's rich block goes away with the event: keep a copy of our own (none if it cannot be had).
    s->search_rich[i] = ccord_message_rich_clone(m.rich, nullptr);
    m.rich = s->search_rich[i];
    if (m.reactions && m.reaction_kind_count > 0) {
      size_t rcount = m.reaction_kind_count;
      ccord_reaction *rs = ccord_arena_alloc(&s->search_arena, rcount * sizeof *rs, _Alignof(ccord_reaction));
      if (rs) {
        for (size_t r = 0; r < rcount; r++) {
          rs[r] = m.reactions[r];
          rs[r].emoji = dup_in_search_arena(&s->search_arena, m.reactions[r].emoji);
        }
        m.reactions = rs;
      } else {
        m.reactions = nullptr;
        m.reaction_kind_count = 0;
      }
    } else {
      m.reactions = nullptr;
      m.reaction_kind_count = 0;
    }
    s->search_results[i] = m;
  }
  s->search_count = ev->message_count;
}


static ccord_status session_b64_decode(const char *b64, size_t len, uint8_t **out, size_t *out_len, ccord_error *err)
{
  *out = nullptr;
  *out_len = 0;
  if (len > 0 && !b64) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null base64");
  if (len > CCORD_PROTO_MAX_INPUT * 2u) return ccord_error_set(err, CCORD_ERR_RANGE, "session: base64 too large");
  if (len == 0) {
    *out = ccord_malloc(1);
    if (!*out) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
    return CCORD_OK;
  }
  uint8_t *raw = ccord_base64_decode_alloc(b64, len, out_len);
  if (!raw) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid base64");
  *out = raw;
  return CCORD_OK;
}

// Keeps the lossless wire bytes of the last settings-proto answer: the transport may hand them over directly
// (raw_proto), otherwise they are the base64 "settings" string of the GET body. Favorite-GIF edits re-encode
// from this cache, so versions, frecency and unknown fields survive a PATCH.
void sess_cache_settings_proto(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error err;
  if (ev->raw_proto && ev->raw_proto_len > 0) {
    if (ccord_store_set_raw_settings_proto(s->store, ev->raw_proto, ev->raw_proto_len, &err) != CCORD_OK) {
      CCORD_LOG_W("session: settings proto cache rejected: %s", err.message);
    }
    return;
  }
  ccord_arena tmp;
  ccord_arena_init(&tmp, 0);
  ccord_json_limits lim = {.max_depth = 8, .max_nodes = 256, .max_string = CCORD_PROTO_MAX_INPUT * 2u};
  const ccord_json *root = ccord_json_parse(&tmp, ev->expr_body, ev->expr_body_len, &lim, &err);
  const ccord_json *node =
    root && ccord_json_type_of(root) == CCORD_JSON_OBJECT ? ccord_json_get(root, "settings") : nullptr;
  size_t blen = 0;
  const char *b64 = node ? ccord_json_string(node, &blen) : nullptr;
  if (b64) {
    uint8_t *raw = nullptr;
    size_t rlen = 0;
    if (session_b64_decode(b64, blen, &raw, &rlen, &err) == CCORD_OK) {
      if (ccord_store_set_raw_settings_proto(s->store, raw, rlen, &err) != CCORD_OK) {
        CCORD_LOG_W("session: settings proto cache rejected: %s", err.message);
      }
      ccord_free(raw);
    }
  }
  ccord_arena_free(&tmp);
}

// Re-encodes the store's favorite GIF list over the cached settings-proto wire bytes (lossless: every other
// top-level record is copied verbatim) and PATCHes it. The cache moves to the sent bytes only once the request
// went out; the server's answer refreshes it again through the SETTINGS_PROTO ingest.
// Debug aid for "my favourite GIFs are out of order": the count and the first 10 favourites as the picker draws them,
// each as order:hash (an FNV-1a hash of the GIF page URL, so the log holds no URL).
static void log_favorite_gifs(const ccord_store *store)
{
  char line[256];
  size_t n = ccord_store_favorite_gif_count(store), off = 0;
  line[0] = '\0';
  for (size_t i = 0; i < n && i < 10u && off < sizeof line; i++) {
    const ccord_gif_item *it = ccord_store_favorite_gif_at(store, i);
    if (!it) break;
    uint32_t h = 2166136261u;
    for (const char *p = it->url; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    int w = snprintf(line + off, sizeof line - off, " %u:%08x", (unsigned)it->order, (unsigned)h);
    if (w < 0 || (size_t)w >= sizeof line - off) break;
    off += (size_t)w;
  }
  CCORD_LOG_D("session: favourite gifs loaded: count=%zu first (order:urlhash):%s", n, line);
}

static session_fav_pending *fav_pending_find(ccord_session *s, const char *url)
{
  for (size_t i = 0; i < s->nfav_pending; i++) {
    if (strcmp(s->fav_pending[i].url, url) == 0) return &s->fav_pending[i];
  }
  return nullptr;
}

static void fav_pending_drop(ccord_session *s, size_t i)
{
  ccord_free(s->fav_pending[i].url);
  memmove(&s->fav_pending[i], &s->fav_pending[i + 1], (s->nfav_pending - i - 1u) * sizeof s->fav_pending[0]);
  s->nfav_pending--;
}

// Records a local toggle (latest one per URL wins). When the table is full the oldest record is evicted.
static void fav_pending_note(ccord_session *s, const char *url, bool starred)
{
  session_fav_pending *p = fav_pending_find(s, url);
  if (!p) {
    char *copy = ccord_strndup(url, strlen(url));
    if (!copy) return;   // out of memory: no shield, the pre-existing additive behaviour of the next load stays
    if (s->nfav_pending == SESSION_FAV_PENDING_MAX) fav_pending_drop(s, 0);
    p = &s->fav_pending[s->nfav_pending++];
    p->url = copy;
  }
  p->starred = starred;
  p->at_ms = ccord_clock_ms();
}

// Makes the store's favourite GIFs equal the wire list `ux` (authoritative: favorite_gifs was present), except for local
// toggles that the server has not confirmed yet. Confirmed or expired toggles are forgotten. `authoritative` false (a
// partial gateway delta) only adds and re-orders: absence from a delta proves nothing.
void sess_apply_favorite_gifs_wire(ccord_session *s, const ccord_user_expressions *ux, bool authoritative)
{
  uint64_t now = ccord_clock_ms();
  for (size_t i = s->nfav_pending; i > 0; i--) {
    if (now - s->fav_pending[i - 1u].at_ms > SESSION_FAV_PENDING_TTL_MS) fav_pending_drop(s, i - 1u);
  }
  // ux.gifs is lodash sortBy("order") (ascending, ties in wire order). Discord then calls .reverse() on it, so
  // the store is fed from the end: highest order first, and tied entries in reverse wire order.
  for (size_t k = 0; k < ux->gif_count; k++) {
    size_t i = ux->gif_count - 1u - k;
    ccord_gif_item g = {0};
    if (ux->gifs[i].url) strncpy(g.url, ux->gifs[i].url, sizeof g.url - 1);
    if (ux->gifs[i].src) strncpy(g.src, ux->gifs[i].src, sizeof g.src - 1);
    g.width = ux->gifs[i].width ? ux->gifs[i].width : 320;
    g.height = ux->gifs[i].height ? ux->gifs[i].height : 240;
    g.format = ux->gifs[i].format;   // wire values kept so a later PATCH re-encodes them losslessly
    g.order = ux->gifs[i].order;
    if (!g.url[0]) continue;
    const session_fav_pending *p = fav_pending_find(s, g.url);
    if (p && !p->starred) continue;   // unstarred here, not yet confirmed: an older answer must not bring it back
    if (!ccord_store_is_favorite_gif(s->store, g.url)) (void)ccord_store_toggle_favorite_gif(s->store, &g);
    // The wire order is authoritative: a new favorite keeps it (not the next free order), and an existing
    // one that a settings update re-orders moves to its new position.
    (void)ccord_store_set_favorite_gif_order(s->store, g.url, g.order);
  }
  if (ux->has_favorite_gifs && authoritative) {
    // Unstarred on another device: in the store but not in the authoritative list.
    for (size_t i = ccord_store_favorite_gif_count(s->store); i > 0; i--) {
      const ccord_gif_item *it = ccord_store_favorite_gif_at(s->store, i - 1u);
      if (!it) continue;
      bool on_wire = false;
      for (size_t k = 0; k < ux->gif_count && !on_wire; k++) on_wire = ux->gifs[k].url && strcmp(ux->gifs[k].url, it->url) == 0;
      if (on_wire) continue;
      const session_fav_pending *p = fav_pending_find(s, it->url);
      if (p && p->starred) continue;   // starred here, PATCH not confirmed yet: keep it, the next PATCH carries it
      ccord_gif_item gone = *it;       // by value: the toggle shifts the store's array under `it`
      (void)ccord_store_toggle_favorite_gif(s->store, &gone);
    }
    // A toggle is confirmed once the list agrees with it.
    for (size_t i = s->nfav_pending; i > 0; i--) {
      bool on_wire = false;
      for (size_t k = 0; k < ux->gif_count && !on_wire; k++) {
        on_wire = ux->gifs[k].url && strcmp(ux->gifs[k].url, s->fav_pending[i - 1u].url) == 0;
      }
      if (on_wire == s->fav_pending[i - 1u].starred) fav_pending_drop(s, i - 1u);
    }
  }
  if (ux->gif_count > 0) log_favorite_gifs(s->store);
}

// Another client changed the account's type-2 settings (gateway USER_SETTINGS_PROTO_UPDATE): favourite GIFs follow it
// through the same path as a settings-proto answer. A whole (non-partial) proto also becomes the base of our next PATCH.
// Only a non-partial proto that carries favorite_gifs can remove a favourite; a delta, an absent field or bytes that do
// not decode remove nothing.
void session_apply_frecency_update(ccord_session *s, const ccord_net_event *ev)
{
  ccord_error err;
  ccord_user_expressions ux;
  if (ccord_user_expressions_decode(ev->raw_proto, ev->raw_proto_len, &ux, &err) != CCORD_OK) {
    CCORD_LOG_W("session: settings proto update undecodable: %s", err.message);
    return;
  }
  if (!ev->settings_proto1_partial &&
      ccord_store_set_raw_settings_proto(s->store, ev->raw_proto, ev->raw_proto_len, &err) != CCORD_OK) {
    CCORD_LOG_W("session: settings proto cache rejected: %s", err.message);
  }
  sess_apply_favorite_gifs_wire(s, &ux, !ev->settings_proto1_partial);
  ccord_user_expressions_free(&ux);
}

static ccord_status session_patch_favorite_gifs(ccord_session *s, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  size_t n = ccord_store_favorite_gif_count(s->store);
  ccord_proto_gif *gifs = nullptr;
  if (n > 0) {
    gifs = ccord_alloc_array(n, sizeof *gifs);
    if (!gifs) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
    for (size_t i = 0; i < n; i++) {
      const ccord_gif_item *it = ccord_store_favorite_gif_at(s->store, i);
      gifs[i] = (ccord_proto_gif){.url = it->url, .src = it->src, .format = it->format,
                                  .width = it->width, .height = it->height, .order = it->order};
    }
  }
  size_t existing_len = 0;
  const uint8_t *existing = ccord_store_get_raw_settings_proto(s->store, &existing_len);
  uint8_t *out = nullptr;
  size_t out_len = 0;
  ccord_status st = ccord_user_expressions_encode_gifs(gifs, n, existing, existing_len, &out, &out_len, err);
  ccord_free(gifs);
  if (st != CCORD_OK) return st;
  st = ccord_transport_patch_settings_proto(s->transport, out, out_len, err);
  if (st == CCORD_OK) {
    ccord_error ignored;
    if (ccord_store_set_raw_settings_proto(s->store, out, out_len, &ignored) != CCORD_OK) {
      CCORD_LOG_W("session: settings proto cache rejected: %s", ignored.message);
    }
  }
  ccord_free(out);
  return st;
}

// Voice disconnect (or a move to another channel): leave every activity instance of the old channel that still
// lists us as a participant, so no ghost participant is left behind. Fire-and-forget: each store row disappears
// when Discord's instance update confirms it. The server may already have removed us; then nothing is sent.
void session_leave_channel_activities(ccord_session *s, ccord_id channel_id)
{
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE) return;
  size_t n = ccord_store_activity_instance_count(s->store, channel_id);
  for (size_t i = 0; i < n; i++) {
    const ccord_activity_instance *row = ccord_store_activity_instance_at(s->store, channel_id, i);
    if (!row) break;
    ccord_activity_instance inst = *row;   // copy: the send below must not borrow a row it could invalidate
    bool participant = false;
    for (size_t p = 0; p < inst.participant_count; p++) {
      if (inst.participants[p] == self) {
        participant = true;
        break;
      }
    }
    if (!participant) continue;
    ccord_id location = inst.location_id != CCORD_ID_NONE ? inst.location_id : inst.channel_id;
    ccord_admin_request req;
    ccord_error err;
    if (ccord_admin_activity_leave(&req, inst.application_id, location, inst.instance_id, &err) != CCORD_OK) {
      CCORD_LOG_W("session: activity leave request rejected: %s", err.message);
      continue;
    }
    if (ccord_session_admin(s, &req, &err) != CCORD_OK) {
      CCORD_LOG_W("session: activity leave failed: %s", err.message);
    }
  }
}

// The own user's "Playing X" line in the store. Discord does not reliably send a user its own PRESENCE_UPDATE, so an
// activity that ended elsewhere (the RPC client left, another session stopped it) would stay shown forever: the
// events that carry the account's own activity (SESSIONS_REPLACE, READY) and our own clear of the Rich Presence set
// or clear it here. Only the activity fields change; presence and custom status stay as the store has them.
session_guild_profile_cache *sess_guild_profile_for(ccord_session *s, ccord_id guild_id)
{
  for (size_t i = 0; i < s->nguild_profiles; i++) {
    if (s->guild_profiles[i].guild_id == guild_id) {
      s->guild_profiles[i].last_use = ++s->cache_tick;
      return &s->guild_profiles[i];
    }
  }
  session_guild_profile_cache *ent;
  if (s->nguild_profiles >= SESSION_GUILD_PROFILE_CACHE_MAX) {
    size_t victim = SIZE_MAX;
    for (size_t i = 0; i < s->nguild_profiles; i++) {
      if (s->guild_profiles[i].loading) continue;
      if (victim == SIZE_MAX || s->guild_profiles[i].last_use < s->guild_profiles[victim].last_use) victim = i;
    }
    if (victim == SIZE_MAX) {
      victim = 0;
      for (size_t i = 1; i < s->nguild_profiles; i++) {
        if (s->guild_profiles[i].last_use < s->guild_profiles[victim].last_use) victim = i;
      }
    }
    ent = &s->guild_profiles[victim];
  } else {
    if (s->nguild_profiles == s->capguild_profiles) {
      size_t ncap = s->capguild_profiles ? s->capguild_profiles * 2u : 8u;
      if (ncap > SESSION_GUILD_PROFILE_CACHE_MAX) ncap = SESSION_GUILD_PROFILE_CACHE_MAX;
      session_guild_profile_cache *g = ccord_realloc_array(s->guild_profiles, ncap, sizeof *g);
      if (!g) return nullptr;
      s->guild_profiles = g;
      s->capguild_profiles = ncap;
    }
    ent = &s->guild_profiles[s->nguild_profiles++];
  }
  memset(ent, 0, sizeof *ent);
  ent->guild_id = guild_id;
  ent->last_use = ++s->cache_tick;
  return ent;
}


static bool str_eq_ascii_ci(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
    if (ca != cb) return false;
  }
  return *a == '\0' && *b == '\0';
}

static ccord_id resolve_search_user(const ccord_store *st, ccord_id guild_id, const char *needle)
{
  if (!st || !needle || needle[0] == '\0') return CCORD_ID_NONE;
  if (str_eq_ascii_ci(needle, "me")) return ccord_store_self_id(st);

  bool in_guild = guild_id != CCORD_ID_NONE && guild_id != CCORD_DM_GUILD_ID;
  // Pass 1: exact case-insensitive match on handle, server nick (in guild), or display name.
  if (in_guild) {
    size_t nm = ccord_store_member_count(st, guild_id);
    for (size_t i = 0; i < nm; i++) {
      const ccord_member *m = ccord_store_member_at(st, guild_id, i);
      if (!m) continue;
      if (m->nick[0] != '\0' && str_eq_ascii_ci(m->nick, needle)) return m->user_id;
      const ccord_user *u = ccord_store_user(st, m->user_id);
      if (u && ((u->handle && str_eq_ascii_ci(u->handle, needle)) ||
                (u->name && str_eq_ascii_ci(u->name, needle)))) {
        return u->id;
      }
    }
  }
  size_t nu = ccord_store_user_count(st);
  for (size_t i = 0; i < nu; i++) {
    const ccord_user *u = ccord_store_user_at(st, i);
    if (!u) continue;
    if ((u->handle && str_eq_ascii_ci(u->handle, needle)) ||
        (u->name && str_eq_ascii_ci(u->name, needle))) {
      return u->id;
    }
  }
  // Pass 2: case-insensitive substring match (guild members first, then known users).
  if (in_guild) {
    size_t nm = ccord_store_member_count(st, guild_id);
    for (size_t i = 0; i < nm; i++) {
      const ccord_member *m = ccord_store_member_at(st, guild_id, i);
      if (!m) continue;
      if (m->nick[0] != '\0' && ccord_search_contains_ci(m->nick, needle)) return m->user_id;
      const ccord_user *u = ccord_store_user(st, m->user_id);
      if (u && ((u->handle && ccord_search_contains_ci(u->handle, needle)) ||
                (u->name && ccord_search_contains_ci(u->name, needle)))) {
        return u->id;
      }
    }
  }
  for (size_t i = 0; i < nu; i++) {
    const ccord_user *u = ccord_store_user_at(st, i);
    if (!u) continue;
    if ((u->handle && ccord_search_contains_ci(u->handle, needle)) ||
        (u->name && ccord_search_contains_ci(u->name, needle))) {
      return u->id;
    }
  }
  return CCORD_ID_NONE;
}

static ccord_id resolve_search_channel(const ccord_store *st, ccord_id guild_id, const char *needle)
{
  if (!st || guild_id == CCORD_ID_NONE || !needle || needle[0] == '\0') return CCORD_ID_NONE;
  size_t nc = ccord_store_channel_count(st, guild_id);
  for (size_t i = 0; i < nc; i++) {
    const ccord_channel *c = ccord_store_channel_at(st, guild_id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->name && str_eq_ascii_ci(c->name, needle)) return c->id;
  }
  for (size_t i = 0; i < nc; i++) {
    const ccord_channel *c = ccord_store_channel_at(st, guild_id, i);
    if (c && c->kind == CCORD_CHANNEL_TEXT && c->name && ccord_search_contains_ci(c->name, needle)) return c->id;
  }
  return CCORD_ID_NONE;
}

ccord_status ccord_session_search_messages_sorted(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                                  const char *query, size_t query_len, uint32_t offset,
                                                  ccord_search_sort sort, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!query || query_len == 0 || sess_blank(query, query_len) ||
      memchr(query, 0, query_len) || !ccord_utf8_valid_text(query, query_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid search query");
  }
  if (guild_id == CCORD_ID_NONE && channel_id == CCORD_ID_NONE) {
    channel_id = ccord_store_selected_channel(s->store);
    guild_id = ccord_store_selected_guild(s->store);
  }
  ccord_search_query sq;
  ccord_search_parse(query, query_len, &sq);
  sq.offset = offset;
  if (sq.sort == CCORD_SEARCH_SORT_NEW && sort != CCORD_SEARCH_SORT_NEW) sq.sort = sort;
  if (sq.author_id == CCORD_ID_NONE && sq.from[0] != '\0') {
    sq.author_id = resolve_search_user(s->store, guild_id, sq.from);
  }
  if (sq.mentions_id == CCORD_ID_NONE && sq.mentions[0] != '\0') {
    sq.mentions_id = resolve_search_user(s->store, guild_id, sq.mentions);
  }
  if (sq.channel_id == CCORD_ID_NONE && sq.in_channel[0] != '\0' && guild_id != CCORD_ID_NONE) {
    sq.channel_id = resolve_search_channel(s->store, guild_id, sq.in_channel);
  }
  ccord_status st = ccord_transport_search_messages_ex(s->transport, guild_id, channel_id, &sq, err);
  if (st == CCORD_OK) s->search_loading = true;
  return st;
}

ccord_status ccord_session_search_messages_page(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                                const char *query, size_t query_len, uint32_t offset,
                                                ccord_error *err)
{
  return ccord_session_search_messages_sorted(s, guild_id, channel_id, query, query_len, offset, CCORD_SEARCH_SORT_NEW, err);
}

ccord_status ccord_session_search_messages(ccord_session *s, ccord_id guild_id, ccord_id channel_id,
                                           const char *query, size_t query_len, ccord_error *err)
{
  return ccord_session_search_messages_page(s, guild_id, channel_id, query, query_len, 0u, err);
}

size_t ccord_session_search_result_count(const ccord_session *s)
{
  return s ? s->search_count : 0;
}

const ccord_message *ccord_session_search_result_at(const ccord_session *s, size_t index)
{
  if (!s || index >= s->search_count) return nullptr;
  return &s->search_results[index];
}

const ccord_message *ccord_session_search_results(const ccord_session *s, size_t *out_count)
{
  if (out_count) *out_count = s ? s->search_count : 0;
  return s ? s->search_results : nullptr;
}

uint32_t ccord_session_search_total_results(const ccord_session *s)
{
  return s ? s->search_total_results : 0u;
}

uint32_t ccord_session_search_offset(const ccord_session *s)
{
  return s ? s->search_offset : 0u;
}

bool ccord_session_search_loading(const ccord_session *s)
{
  return s && s->search_loading;
}

// What the roles Discord told us say about a role request, but only when they are complete: with partial data
// everything is left to Discord (it has the final word). Returns CCORD_PERM_ALLOWED when nothing is certain.

ccord_status ccord_session_request_reaction_users(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                                  const char *emoji, uint32_t limit, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: reaction user ids");
  }
  if (!emoji || emoji[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "session: reaction emoji");
  size_t elen = strlen(emoji);
  if (elen > CCORD_EMOJI_MAX_BYTES || !ccord_utf8_valid_text(emoji, elen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: reaction emoji text");
  }
  if (!ccord_store_channel(s->store, channel_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  }
  if (s->react_users_loading && s->react_users_channel_id == channel_id &&
      s->react_users_message_id == message_id && strcmp(s->react_users_emoji, emoji) == 0) {
    return CCORD_OK;
  }
  sess_clear_reaction_users(s);
  s->react_users_channel_id = channel_id;
  s->react_users_message_id = message_id;
  memcpy(s->react_users_emoji, emoji, elen);
  s->react_users_emoji[elen] = '\0';
  s->react_users_loading = true;
  ccord_status st =
    ccord_transport_request_reaction_users(s->transport, channel_id, message_id, emoji, elen, limit ? limit : 50u, err);
  if (st != CCORD_OK) s->react_users_loading = false;
  return st;
}

const ccord_user *ccord_session_reaction_users(const ccord_session *s, ccord_id *out_channel_id,
                                               ccord_id *out_message_id, const char **out_emoji,
                                               size_t *out_count, bool *out_loading)
{
  if (out_channel_id) *out_channel_id = s ? s->react_users_channel_id : CCORD_ID_NONE;
  if (out_message_id) *out_message_id = s ? s->react_users_message_id : CCORD_ID_NONE;
  if (out_emoji) *out_emoji = s ? s->react_users_emoji : "";
  if (out_count) *out_count = s ? s->react_user_count : 0u;
  if (out_loading) *out_loading = s ? s->react_users_loading : false;
  return s ? s->react_users : nullptr;
}


ccord_status ccord_session_request_expression_data(ccord_session *s, ccord_expr_kind kind, const char *arg, bool force,
                                                   ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (kind >= CCORD_EXPR_KIND_COUNT) return ccord_error_set(err, CCORD_ERR_INVALID, "session: bad expression kind");
  if (!arg) arg = "";
  ccord_expr_blob_state st = ccord_store_expr_blob_state(s->store, kind, arg);
  if (st == CCORD_EXPR_BLOB_PENDING || (st == CCORD_EXPR_BLOB_READY && !force)) return CCORD_OK;
  if (s->stale) {   // background load: remember it, READY replays it
    size_t alen = strlen(arg);
    bool known = false;
    for (size_t i = 0; i < s->nstale_expr; i++) {
      if (s->stale_expr[i].kind == kind && strcmp(s->stale_expr[i].arg, arg) == 0) known = true;
    }
    if (!known && alen < sizeof s->stale_expr[0].arg && s->nstale_expr < 8) {
      s->stale_expr[s->nstale_expr].kind = kind;
      memcpy(s->stale_expr[s->nstale_expr].arg, arg, alen + 1u);
      s->nstale_expr++;
    }
    return CCORD_OK;
  }
  ccord_status rc = ccord_transport_request_expression_data(s->transport, kind, arg, strlen(arg), err);
  ccord_error ignored;
  if (rc == CCORD_OK) {
    ccord_store_expr_blob_mark_pending(s->store, kind, arg);
  } else {
    (void)ccord_store_set_expr_blob(s->store, kind, arg, nullptr, 0, false, &ignored);   // surface as FAILED, no retry storm
  }
  return rc;
}

ccord_status ccord_session_send_sticker(ccord_session *s, ccord_id channel_id, ccord_id sticker_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (sticker_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: sticker id");
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "unknown channel");
  if (!session_channel_has_chat(ch)) return ccord_error_set(err, CCORD_ERR_INVALID, "channel has no chat");
  if (ch->guild_id == CCORD_DM_GUILD_ID && ch->is_message_request) {
    return ccord_error_set(err, CCORD_ERR_PERMISSION, "Accept the message request before replying.");
  }
  ccord_status gate = session_screening_gate(s, ch, err);
  if (gate != CCORD_OK) return gate;
  ccord_status tlock = sess_check_thread_lock(s, ch, err);
  if (tlock != CCORD_OK) return tlock;
  return ccord_transport_send_sticker(s->transport, channel_id, sticker_id, err);
}

ccord_status ccord_session_toggle_favorite_gif(ccord_session *s, const ccord_gif_item *gif, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !gif) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (gif->url[0] == '\0' || !memchr(gif->url, '\0', sizeof gif->url) || !memchr(gif->src, '\0', sizeof gif->src)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid favorite GIF URL");
  }
  if (!ccord_utf8_valid_text(gif->url, strlen(gif->url)) ||
      (gif->src[0] != '\0' && !ccord_utf8_valid_text(gif->src, strlen(gif->src)))) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid favorite GIF URL");
  }
  ccord_gif_item prev_item = *gif;
  bool was_fav = false;
  for (size_t i = 0, n = ccord_store_favorite_gif_count(s->store); i < n; i++) {
    const ccord_gif_item *it = ccord_store_favorite_gif_at(s->store, i);
    if (it && strcmp(it->url, gif->url) == 0) {
      prev_item = *it;
      was_fav = true;
      break;
    }
  }
  bool now_fav = ccord_store_toggle_favorite_gif(s->store, gif);
  if (was_fav == now_fav) {
    return ccord_error_set(err, ccord_store_favorite_gif_count(s->store) > 0u ? CCORD_ERR_RANGE : CCORD_ERR_NOMEM,
                           "Favorite GIF limit reached.");
  }
  ccord_status st = session_patch_favorite_gifs(s, err);
  if (st != CCORD_OK) {
    (void)ccord_store_toggle_favorite_gif(s->store, &prev_item);
  } else {
    fav_pending_note(s, gif->url, now_fav);
  }
  return st;
}

ccord_status ccord_session_request_forum_posts(ccord_session *s, const ccord_forum_query *query, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !query) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (query->forum_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: forum id");
  if (!ccord_store_channel_is_forum(s->store, query->forum_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown forum channel");
  }
  if (query->name && query->name[0] != '\0') {
    size_t nlen = strlen(query->name);
    if (!ccord_utf8_valid_text(query->name, nlen)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid forum search query");
    }
  }
  const ccord_forum_state *prev = ccord_store_forum_state(s->store, query->forum_id);
  if (prev && prev->loading) return CCORD_OK;

  ccord_status st = ccord_transport_request_forum_posts(s->transport, query, err);
  if (st == CCORD_OK) {
    ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
    fst.loading = true;
    fst.error[0] = '\0';
    ccord_error ignored;
    (void)ccord_store_forum_set_state(s->store, query->forum_id, &fst, &ignored);
  }
  return st;
}

ccord_status ccord_session_create_forum_post(ccord_session *s, ccord_id forum_id,
                                             const char *title, size_t title_len,
                                             const char *content, size_t content_len,
                                             const ccord_id *tag_ids, size_t tag_count,
                                             ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (forum_id == CCORD_ID_NONE || !ccord_store_channel_is_forum(s->store, forum_id)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown forum channel");
  }
  if (!title || title_len == 0 || title_len > CCORD_FORUM_TITLE_BYTES || sess_blank(title, title_len) ||
      memchr(title, 0, title_len) || !ccord_utf8_valid_text(title, title_len)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid post title");
  }
  ccord_status tst = sess_validate_text(content, content_len, err);
  if (tst != CCORD_OK) return tst;
  if (tag_count > CCORD_FORUM_MAX_APPLIED_TAGS || (tag_count > 0 && !tag_ids)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid post tags");
  }
  const ccord_forum_meta *meta = ccord_store_forum_meta(s->store, forum_id);
  if (meta && meta->require_tag && tag_count == 0) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: at least one tag is required");
  }
  const ccord_forum_state *prev = ccord_store_forum_state(s->store, forum_id);
  if (prev && prev->creating) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: post creation already in flight");
  }
  ccord_status st = ccord_transport_create_forum_post(s->transport, forum_id, title, title_len,
                                                      content, content_len, tag_ids, tag_count, err);
  if (st == CCORD_OK) {
    ccord_forum_state fst = prev ? *prev : (ccord_forum_state){0};
    fst.creating = true;
    fst.error[0] = '\0';
    ccord_error ignored;
    (void)ccord_store_forum_set_state(s->store, forum_id, &fst, &ignored);
  }
  return st;
}

ccord_status ccord_session_open_forum_post(ccord_session *s, ccord_id thread_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (thread_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid thread id");
  ccord_status st = ccord_store_forum_open_post(s->store, thread_id, err);
  if (st == CCORD_OK) {
    ccord_id gid = ccord_store_selected_guild(s->store);
    if (gid != CCORD_ID_NONE && !s->stale && CCORD_TRANSPORT_OPTIONAL(s->transport, select_channel)) {
      ccord_error hook_err;
      (void)s->transport->vt->select_channel(s->transport, gid, thread_id, &hook_err);
    }
    s->retried_channel = CCORD_ID_NONE;
    sess_request_history_once(s, thread_id);
  }
  return st;
}

ccord_status ccord_session_vote_poll(ccord_session *s, ccord_id channel_id, ccord_id message_id,
                                     const uint32_t *answer_ids, size_t answer_count, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (channel_id == CCORD_ID_NONE || message_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid poll ids");
  }
  if (answer_count > CCORD_RICH_MAX_POLL_ANSWERS) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "session: too many poll answers");
  }
  if (answer_count > 0 && !answer_ids) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: null poll answers");
  }
  const ccord_message *m = ccord_store_find_message(s->store, channel_id, message_id);
  if (!m) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown poll message");
  if (!m->rich || !m->rich->poll.present) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: message has no poll");
  }
  if (m->rich->poll.finalized) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: poll is finalized");
  }
  if (!m->rich->poll.multiselect && answer_count > 1) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: poll allows one answer");
  }
  return ccord_transport_vote_poll(s->transport, channel_id, message_id, answer_ids, answer_count, err);
}

ccord_status ccord_session_send_interaction(ccord_session *s, const ccord_interaction_request *req,
                                            ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!req || req->channel_id == CCORD_ID_NONE || req->application_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid interaction request");
  }
  if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND ||
      req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE) {
    if (req->command_id == CCORD_ID_NONE || !req->command_name || req->command_name[0] == '\0' ||
        req->command_type > 3u || req->option_count > CCORD_SLASH_MAX_OPTIONS) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid slash command interaction");
    }
    if (req->command_type >= 2u) {
      if (req->type != CCORD_INTERACTION_APPLICATION_COMMAND || req->target_id == CCORD_ID_NONE ||
          session_is_local_echo_id(req->target_id) || req->option_count > 0 ||
          (req->subcommand && req->subcommand[0] != '\0') ||
          (req->subcommand_group && req->subcommand_group[0] != '\0')) {
        return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid context menu command interaction");
      }
    } else if (req->subcommand_group && req->subcommand_group[0] != '\0' &&
               (!req->subcommand || req->subcommand[0] == '\0')) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: subcommand_group requires subcommand");
    }
    if (req->option_count > 0 && (!req->option_names || !req->option_kinds || !req->option_values)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: null slash command options");
    }
    if (req->type == CCORD_INTERACTION_APPLICATION_COMMAND_AUTOCOMPLETE &&
        (req->focused_option < 0 || (size_t)req->focused_option >= req->option_count)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid focused autocomplete option");
    }
  } else if (req->type == CCORD_INTERACTION_MESSAGE_COMPONENT ||
             req->type == CCORD_INTERACTION_MODAL_SUBMIT) {
    if (!req->custom_id || req->custom_id[0] == '\0') {
      return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid interaction custom_id");
    }
  } else {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: unsupported interaction type");
  }
  const ccord_channel *ch = ccord_store_channel(s->store, req->channel_id);
  if (!ch) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: unknown channel");
  ccord_interaction_request filled = *req;
  if (ch->guild_id == CCORD_DM_GUILD_ID || filled.guild_id == CCORD_DM_GUILD_ID) {
    filled.guild_id = CCORD_ID_NONE;
  } else if (filled.guild_id == CCORD_ID_NONE) {
    filled.guild_id = ch->guild_id;
  }
  ccord_status st = ccord_transport_send_interaction(s->transport, &filled, err);
  if (st == CCORD_OK && filled.type == CCORD_INTERACTION_APPLICATION_COMMAND) {
    s->slash_choice_count = 0;
  }
  return st;
}

bool ccord_session_pop_interaction_modal(ccord_session *s, ccord_interaction_modal *out)
{
  if (!s || !s->modals || s->modal_count == 0) return false;
  if (out) *out = s->modals[s->modal_head];
  s->modal_head = (s->modal_head + 1u) % SESSION_MAX_MODALS;
  s->modal_count--;
  return true;
}

bool ccord_session_ensure_slash_commands(ccord_session *s, ccord_id guild_id, ccord_id channel_id, uint64_t now_ms)
{
  if (!s || channel_id == CCORD_ID_NONE) return false;
  const ccord_channel *ch = ccord_store_channel(s->store, channel_id);
  if (!ch || ch->kind != CCORD_CHANNEL_TEXT) return false;
  ccord_id gid = (guild_id != CCORD_ID_NONE) ? guild_id : ch->guild_id;
  if (gid == CCORD_ID_NONE) return false;
  if (s->stale) {   // background load: READY replays the latest one
    s->stale_slash_guild = gid;
    s->stale_slash_channel = channel_id;
    return false;
  }
  session_slash_cache *ent = sess_slash_cache_for(s, gid);
  if (!ent) return false;
  uint64_t t = now_ms ? now_ms : 1u;
  if (ent->loaded && t < ent->fetched_ms + SESSION_SLASH_TTL_MS) return false;
  if (ent->in_flight && t < ent->sent_ms + SESSION_SLASH_WAIT_MS) return false;
  if (ent->sent_ms != 0 && t < ent->sent_ms + SESSION_SLASH_RETRY_MS) return false;
  ent->sent_ms = t;
  ccord_error err;
  if (ccord_transport_request_slash_commands(s->transport, gid, channel_id, &err) != CCORD_OK) {
    ent->in_flight = false;
    return false;
  }
  ent->in_flight = true;
  return true;
}

const ccord_slash_choice *ccord_session_slash_choices(const ccord_session *s, size_t *out_count, uint64_t *out_seq)
{
  if (out_count) *out_count = s ? s->slash_choice_count : 0;
  if (out_seq) *out_seq = s ? s->slash_choice_seq : 0;
  return (s && s->slash_choice_count > 0) ? s->slash_choices : nullptr;
}

void ccord_session_clear_slash_choices(ccord_session *s)
{
  if (!s) return;
  s->slash_choice_count = 0;
  s->slash_choice_seq++;
}


ccord_status ccord_session_request_discovery(ccord_session *s, const char *query, uint32_t category_id,
                                             uint32_t offset, uint64_t now_ms, bool force, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const char *q = query ? query : "";
  size_t qlen = strlen(q);
  if (qlen > 100u || memchr(q, 0, qlen) || !ccord_utf8_valid_text(q, qlen)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid discovery query");
  }
  uint64_t t = now_ms ? now_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);

  if (!force) {
    if (s->discovery_cache) {
      for (size_t i = 0; i < SESSION_DISCOVERY_CACHE_SLOTS; i++) {
        const session_discovery_cache_entry *ce = &s->discovery_cache[i];
        if (!ce->valid) continue;
        if (ce->category_id != category_id || ce->offset != offset || strcmp(ce->query, q) != 0) continue;
        if (t >= ce->fetched_ms && t < ce->fetched_ms + CCORD_DISCOVERY_TTL_MS) {
          if (ce->count > 0 && !s->discovery_items) {
            s->discovery_items = ccord_calloc(CCORD_DISCOVERY_PAGE_MAX, sizeof *s->discovery_items);
            if (!s->discovery_items) break;
          }
          s->discovery_count = ce->count;
          s->discovery_total = ce->total;
          s->discovery_offset = ce->offset;
          s->discovery_category_id = ce->category_id;
          memcpy(s->discovery_query, ce->query, strlen(ce->query) + 1u);
          if (ce->count > 0) memcpy(s->discovery_items, ce->items, ce->count * sizeof s->discovery_items[0]);
          s->discovery_loading = false;
          return CCORD_OK;
        }
      }
    }
    if (s->discovery_loading && s->discovery_category_id == category_id && s->discovery_offset == offset &&
        strcmp(s->discovery_query, q) == 0 && t < s->discovery_sent_ms + 15000u) {
      return CCORD_OK;
    }
  }

  s->discovery_category_id = category_id;
  s->discovery_offset = offset;
  memcpy(s->discovery_query, q, qlen + 1u);
  s->discovery_sent_ms = t;
  s->discovery_loading = true;
  ccord_status st = ccord_transport_request_discoverable_guilds(s->transport, q, category_id, offset,
                                                                CCORD_DISCOVERY_PAGE_MAX, err);
  if (st != CCORD_OK) s->discovery_loading = false;
  return st;
}

const ccord_discover_guild *ccord_session_discovery_results(const ccord_session *s, size_t *out_count,
                                                            uint32_t *out_total, uint32_t *out_offset,
                                                            bool *out_loading)
{
  if (out_count) *out_count = (s && s->discovery_items) ? s->discovery_count : 0u;
  if (out_total) *out_total = s ? s->discovery_total : 0u;
  if (out_offset) *out_offset = s ? s->discovery_offset : 0u;
  if (out_loading) *out_loading = s ? s->discovery_loading : false;
  return (s && s->discovery_items && s->discovery_count > 0) ? s->discovery_items : nullptr;
}


// ---- embedded activities ----

ccord_status ccord_session_activity_shelf_fetch(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "Activities need a server.");
  }
  uint64_t now = ccord_clock_ms();
  if (now == 0) now = 1u;
  session_activity_shelf *ent = sess_activity_shelf_for(s, guild_id);
  if (!ent) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  if (ent->loading && ent->sent_ms != 0 && now < ent->sent_ms + SESSION_ACTIVITY_SHELF_WAIT_MS) {
    return CCORD_OK;   // the same guild's request is already awaiting its answer
  }
  if (ent->fetched_ms != 0 && now >= ent->fetched_ms && now < ent->fetched_ms + SESSION_ACTIVITY_SHELF_TTL_MS) {
    return CCORD_OK;   // the cached shelf is still fresh
  }
  ccord_admin_request req;
  ccord_status st = ccord_admin_activity_shelf(&req, guild_id, err);
  if (st != CCORD_OK) return st;
  ent->loading = true;
  ent->sent_ms = now;
  st = ccord_session_admin(s, &req, err);
  if (st != CCORD_OK) {   // refused before it left: keep the cached shelf and let the next ask retry
    ent = sess_activity_shelf_for(s, guild_id);
    if (ent) {
      ent->loading = false;
      ent->sent_ms = 0;
    }
  }
  return st;
}

const ccord_activity_app *ccord_session_activity_shelf(const ccord_session *s, ccord_id guild_id, size_t *out_count)
{
  if (out_count) *out_count = 0;
  if (!s) return nullptr;
  for (size_t i = 0; i < s->nactivity_shelves; i++) {
    if (s->activity_shelves[i].guild_id == guild_id) {
      if (out_count) *out_count = s->activity_shelves[i].count;
      return s->activity_shelves[i].count > 0 ? s->activity_shelves[i].apps : nullptr;
    }
  }
  return nullptr;
}

ccord_status ccord_session_activity_launch(ccord_session *s, ccord_id channel_id, ccord_id application_id,
                                           ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st =
    ccord_admin_activity_launch(&req, channel_id, application_id, SESSION_ACTIVITY_SESSION_PLACEHOLDER, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);   // gates on the voice connection and the permission; the row follows
}

ccord_status ccord_session_activity_leave(ccord_session *s, const ccord_activity_instance *instance, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !instance) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  ccord_id location = instance->location_id != CCORD_ID_NONE ? instance->location_id : instance->channel_id;
  ccord_admin_request req;
  ccord_status st = ccord_admin_activity_leave(&req, instance->application_id, location, instance->instance_id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);   // the local row changes when Discord's instance update arrives
}

const ccord_activity_instance *ccord_session_activity_instances(const ccord_session *s, ccord_id channel_id,
                                                                size_t *out_count)
{
  size_t n = s ? ccord_store_activity_instance_count(s->store, channel_id) : 0u;
  if (out_count) *out_count = n;
  // The store keeps a channel's rows contiguous, so the first row addresses all `n` of them.
  return n > 0 ? ccord_store_activity_instance_at(s->store, channel_id, 0) : nullptr;
}

ccord_status ccord_session_request_scheduled_events(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_event_list(&req, guild_id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

ccord_status ccord_session_rsvp_scheduled_event(ccord_session *s, ccord_id guild_id, ccord_id event_id, bool rsvp,
                                                ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = rsvp ? ccord_admin_event_rsvp_add(&req, guild_id, event_id, err)
                         : ccord_admin_event_rsvp_remove(&req, guild_id, event_id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}


