// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "session_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sess_reset_account_lists(ccord_session *s)
{
  if (!s) return;
  s->auth_session_count = 0;
  s->auth_sessions_state = CCORD_FETCH_IDLE;
  s->auth_sessions_sent_ms = 0;
  s->auth_sessions_fetched_ms = 0;
  s->connection_count = 0;
  s->connections_state = CCORD_FETCH_IDLE;
  s->connections_sent_ms = 0;
  s->connections_fetched_ms = 0;
}


static bool was_profile_requested(const ccord_session *s, ccord_id user_id)
{
  const session_profile_cache *pe = sess_find_profile_cache(s, user_id);
  return pe && (pe->in_flight || pe->loaded);
}

void sess_mark_profile_loaded(ccord_session *s, ccord_id user_id)
{
  session_profile_cache *pe = sess_profile_cache_for(s, user_id);
  if (!pe) return;
  pe->in_flight = false;
  pe->loaded = true;
  uint64_t fms = pe->sent_ms ? pe->sent_ms : ccord_clock_ms();
  pe->fetched_ms = fms ? fms : 1u;
}

void sess_clear_profile_requested(ccord_session *s, ccord_id user_id)
{
  session_profile_cache *pe = sess_find_profile_cache(s, user_id);
  if (pe) pe->in_flight = false;
}

// Invite previews are small and immutable enough to key by code: the cache owns value copies, never store pointers.
session_invite_cache *sess_find_invite_cache(ccord_session *s, const char *code)
{
  if (!s || !s->invite_cache || !code || code[0] == '\0') return nullptr;
  for (size_t i = 0; i < SESSION_INVITE_CACHE_SLOTS; i++) {
    if (s->invite_cache[i].valid && strcmp(s->invite_cache[i].code, code) == 0) return &s->invite_cache[i];
  }
  return nullptr;
}

session_invite_cache *sess_invite_cache_for(ccord_session *s, const char *code)
{
  session_invite_cache *entry = sess_find_invite_cache(s, code);
  if (entry) return entry;
  if (!s || !code || code[0] == '\0') return nullptr;
  if (!s->invite_cache) {
    s->invite_cache = ccord_calloc(SESSION_INVITE_CACHE_SLOTS, sizeof *s->invite_cache);
    if (!s->invite_cache) return nullptr;
  }
  entry = &s->invite_cache[s->invite_cache_rr++ % SESSION_INVITE_CACHE_SLOTS];
  memset(entry, 0, sizeof *entry);
  entry->valid = true;
  (void)snprintf(entry->code, sizeof entry->code, "%s", code);
  return entry;
}


void sess_reset_verification_cache(ccord_session *s)
{
  for (size_t i = 0; i < s->nverification_cache; i++) ccord_arena_free(&s->verification_cache[i].arena);
  s->nverification_cache = 0;
}

session_verification_cache *sess_verification_cache_for(ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nverification_cache; i++) {
    if (s->verification_cache[i].guild_id == guild_id) {
      s->verification_cache[i].last_use = ++s->cache_tick;
      return &s->verification_cache[i];
    }
  }
  session_verification_cache *ent;
  if (s->nverification_cache >= SESSION_VERIFICATION_CACHE_MAX) {
    size_t victim = 0;
    for (size_t i = 1; i < s->nverification_cache; i++) {
      if (s->verification_cache[i].last_use < s->verification_cache[victim].last_use) victim = i;
    }
    ent = &s->verification_cache[victim];
    ccord_arena_free(&ent->arena);
  } else {
    if (s->nverification_cache == s->capverification_cache) {
      size_t ncap = s->capverification_cache ? s->capverification_cache * 2u : 4u;
      if (ncap > SESSION_VERIFICATION_CACHE_MAX) ncap = SESSION_VERIFICATION_CACHE_MAX;
      session_verification_cache *g = ccord_realloc_array(s->verification_cache, ncap, sizeof *g);
      if (!g) return nullptr;
      s->verification_cache = g;
      s->capverification_cache = ncap;
    }
    ent = &s->verification_cache[s->nverification_cache++];
  }
  memset(ent, 0, sizeof *ent);
  ent->guild_id = guild_id;
  ent->last_use = ++s->cache_tick;
  return ent;
}

// Copies one form string into the arena, truncated to the model cap (the parser truncates too; this keeps a hostile
// transport answer from allocating without bound). nullptr reads as "".
static char *verification_str(ccord_arena *a, const char *src, size_t cap)
{
  if (!src) src = "";
  size_t n = strlen(src);
  if (n > cap) n = cap;
  return ccord_arena_strndup(a, src, n);
}

// Deep-copies a GET answer into the entry. The copy is built in a fresh arena first, so an out-of-memory answer
// leaves the previously cached form untouched.
bool sess_cache_verification_form(session_verification_cache *ent, const ccord_member_verification *form)
{
  ccord_arena fresh;
  ccord_arena_init(&fresh, 1024u);
  ccord_member_verification copy = {0};
  bool ok = true;
  copy.version = verification_str(&fresh, form->version, CCORD_MEMBER_VERIFICATION_VERSION_MAX_BYTES);
  copy.description = verification_str(&fresh, form->description, CCORD_MEMBER_VERIFICATION_DESC_MAX_BYTES);
  if (!copy.version || !copy.description) ok = false;
  size_t nf = form->field_count;
  if (nf > CCORD_MEMBER_VERIFICATION_MAX_FIELDS) nf = CCORD_MEMBER_VERIFICATION_MAX_FIELDS;
  for (size_t i = 0; ok && i < nf; i++) {
    const ccord_member_verification_field *f = &form->fields[i];
    ccord_member_verification_field *out = &copy.fields[i];
    out->field_type = f->field_type;
    out->required = f->required;
    out->label = verification_str(&fresh, f->label, CCORD_MEMBER_VERIFICATION_LABEL_MAX_BYTES);
    if (!out->label) { ok = false; break; }
    size_t nv = f->value_count;
    if (nv > CCORD_MEMBER_VERIFICATION_MAX_VALUES) nv = CCORD_MEMBER_VERIFICATION_MAX_VALUES;
    for (size_t v = 0; v < nv; v++) {
      out->values[v] = verification_str(&fresh, f->values[v], CCORD_MEMBER_VERIFICATION_VALUE_MAX_BYTES);
      if (!out->values[v]) { ok = false; break; }
    }
    out->value_count = nv;
  }
  if (!ok) {
    ccord_arena_free(&fresh);
    return false;
  }
  copy.field_count = nf;
  ccord_arena_free(&ent->arena);
  ent->arena = fresh;
  ent->form = copy;
  return true;
}

// A store seeded from disk has a self id but no live connection: nothing may be sent until READY.

static bool fixed_text_equal(const char *a, size_t acap, const char *b, size_t bcap)
{
  const char *ae = memchr(a, '\0', acap);
  const char *be = memchr(b, '\0', bcap);
  if (!ae || !be) return false;
  size_t alen = (size_t)(ae - a);
  size_t blen = (size_t)(be - b);
  return alen == blen && memcmp(a, b, alen) == 0;
}

// A logout ack names exactly the hashes the server closed: drop only those cached rows, in place.
void sess_drop_auth_sessions(ccord_session *s, const char hashes[][64], size_t count)
{
  if (!s || !hashes) return;
  for (size_t i = 0; i < s->auth_session_count;) {
    bool drop = false;
    for (size_t j = 0; j < count && !drop; j++) {
      drop = fixed_text_equal(s->auth_sessions[i].id_hash, sizeof s->auth_sessions[i].id_hash, hashes[j], 64u);
    }
    if (drop) {
      memmove(&s->auth_sessions[i], &s->auth_sessions[i + 1],
              (s->auth_session_count - i - 1u) * sizeof s->auth_sessions[0]);
      s->auth_session_count--;
    } else {
      i++;
    }
  }
}

// A PATCH answer carries one connection: replace the cached row it names, append it when a loaded list
// simply predates it. Before the list is loaded there is nothing to patch, and inventing a one-row list
// would be the fake local state the UI must not show.
void sess_upsert_connection(ccord_session *s, const ccord_connection *c)
{
  if (!s || !c || s->connections_state != CCORD_FETCH_READY) return;
  for (size_t i = 0; i < s->connection_count; i++) {
    if (fixed_text_equal(s->connections[i].type, sizeof s->connections[i].type, c->type, sizeof c->type) &&
        fixed_text_equal(s->connections[i].id, sizeof s->connections[i].id, c->id, sizeof c->id)) {
      s->connections[i] = *c;
      return;
    }
  }
  if (s->connection_count < CCORD_SESSION_MAX_CONNECTIONS) s->connections[s->connection_count++] = *c;
}

void sess_drop_connection(ccord_session *s, const char *name, const char *id)
{
  if (!s || !name || !id) return;
  for (size_t i = 0; i < s->connection_count; i++) {
    if (fixed_text_equal(s->connections[i].type, sizeof s->connections[i].type, name, 32u) &&
        fixed_text_equal(s->connections[i].id, sizeof s->connections[i].id, id, 64u)) {
      memmove(&s->connections[i], &s->connections[i + 1],
              (s->connection_count - i - 1u) * sizeof s->connections[0]);
      s->connection_count--;
      return;
    }
  }
}


// ---- M6 account settings, active devices, and linked connections --------------------------------

ccord_status ccord_session_user_settings_patch(ccord_session *s, const ccord_user_settings_patch *patch,
                                                ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_user_settings_patch(&req, patch, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);   // the store changes only when the full answer arrives
}

static ccord_status fetch_account_list(ccord_session *s, bool sessions, uint64_t now_ms, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  uint64_t now = now_ms ? now_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);
  ccord_fetch_state state = sessions ? s->auth_sessions_state : s->connections_state;
  uint64_t sent = sessions ? s->auth_sessions_sent_ms : s->connections_sent_ms;
  uint64_t fetched = sessions ? s->auth_sessions_fetched_ms : s->connections_fetched_ms;
  if (state == CCORD_FETCH_LOADING && sent != 0 && now < sent + CCORD_SESSION_ACCOUNT_WAIT_MS) {
    return CCORD_OK;   // one identical request is already awaiting its answer
  }
  if (state == CCORD_FETCH_READY && now >= fetched && now < fetched + CCORD_SESSION_ACCOUNT_TTL_MS) {
    return CCORD_OK;   // the list is still fresh
  }
  ccord_admin_request req;
  ccord_status st = sessions ? ccord_admin_auth_sessions_list(&req, err)
                             : ccord_admin_connections_list(&req, err);
  if (st != CCORD_OK) return st;
  if (sessions) {
    s->auth_sessions_state = CCORD_FETCH_LOADING;
    s->auth_sessions_sent_ms = now;
  } else {
    s->connections_state = CCORD_FETCH_LOADING;
    s->connections_sent_ms = now;
  }
  st = ccord_session_admin(s, &req, err);
  if (st != CCORD_OK) {   // the queue refused it: keep the last list and let the next ask retry
    if (sessions) {
      s->auth_sessions_state = CCORD_FETCH_FAILED;
      s->auth_sessions_sent_ms = 0;
    } else {
      s->connections_state = CCORD_FETCH_FAILED;
      s->connections_sent_ms = 0;
    }
  }
  return st;
}

ccord_status ccord_session_fetch_auth_sessions_ex(ccord_session *s, uint64_t now_ms, ccord_error *err)
{
  return fetch_account_list(s, true, now_ms, err);
}

ccord_status ccord_session_fetch_auth_sessions(ccord_session *s, ccord_error *err)
{
  return fetch_account_list(s, true, 0, err);
}

ccord_status ccord_session_fetch_connections_ex(ccord_session *s, uint64_t now_ms, ccord_error *err)
{
  return fetch_account_list(s, false, now_ms, err);
}

ccord_status ccord_session_fetch_connections(ccord_session *s, ccord_error *err)
{
  return fetch_account_list(s, false, 0, err);
}

ccord_status ccord_session_logout_auth_sessions(ccord_session *s, const char hashes[][64], size_t count,
                                                 ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (!hashes || count == 0) return ccord_error_set(err, CCORD_ERR_INVALID, "session: no sessions to log out");
  for (size_t off = 0; off < count;) {
    size_t batch = count - off;
    if (batch > CCORD_ADMIN_SESSION_LOGOUT_MAX) batch = CCORD_ADMIN_SESSION_LOGOUT_MAX;
    ccord_admin_request req;
    ccord_status st = ccord_admin_auth_sessions_logout(&req, hashes + off, batch, err);
    if (st != CCORD_OK) return st;
    st = ccord_session_admin(s, &req, err);
    if (st != CCORD_OK) return st;
    off += batch;
  }
  return CCORD_OK;   // the cached list drops each row when its own 204 ack arrives
}

ccord_status ccord_session_edit_connection(ccord_session *s, const char *name, const char *id,
                                            const ccord_connection_edit *edit, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_connection_edit(&req, name, id, edit, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

ccord_status ccord_session_delete_connection(ccord_session *s, const char *name, const char *id, ccord_error *err)
{
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_connection_delete(&req, name, id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);
}

const ccord_auth_session *ccord_session_auth_sessions(const ccord_session *s, size_t *count)
{
  if (count) *count = s ? s->auth_session_count : 0u;
  return s ? s->auth_sessions : nullptr;
}

const ccord_connection *ccord_session_connections(const ccord_session *s, size_t *count)
{
  if (count) *count = s ? s->connection_count : 0u;
  return s ? s->connections : nullptr;
}

ccord_fetch_state ccord_session_auth_sessions_state(const ccord_session *s)
{
  return s ? s->auth_sessions_state : CCORD_FETCH_IDLE;
}

ccord_fetch_state ccord_session_connections_state(const ccord_session *s)
{
  return s ? s->connections_state : CCORD_FETCH_IDLE;
}


ccord_status ccord_session_open_dm(ccord_session *s, ccord_id recipient_id, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (recipient_id == CCORD_ID_NONE || recipient_id == ccord_store_self_id(s->store)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid DM recipient");
  }
  ccord_status st = ccord_transport_open_dm(s->transport, recipient_id, err);
  if (st == CCORD_OK) {
    ccord_error ignored;
    (void)ccord_session_request_profile(s, recipient_id, &ignored);
  }
  return st;
}

ccord_status ccord_session_request_profile_ex(ccord_session *s, ccord_id user_id, ccord_id guild_id,
                                              uint64_t now_ms, bool force, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid user id");
  if (guild_id == CCORD_DM_GUILD_ID) guild_id = CCORD_ID_NONE;
  uint64_t t = now_ms ? now_ms : (ccord_clock_ms() ? ccord_clock_ms() : 1u);

  session_profile_cache *pe = sess_profile_cache_for(s, user_id);
  if (!force && pe && pe->guild_id == guild_id) {
    if (pe->in_flight && t < pe->sent_ms + 15000u) return CCORD_OK;
    if (pe->loaded && ccord_store_user_profile(s->store, user_id) != nullptr &&
        t >= pe->fetched_ms && t < pe->fetched_ms + CCORD_PROFILE_TTL_MS) {
      return CCORD_OK;
    }
  }
  ccord_status st = ccord_transport_request_profile_ex(s->transport, user_id, guild_id, err);
  if (st == CCORD_OK && pe) {
    pe->guild_id = guild_id;
    pe->sent_ms = t;
    pe->in_flight = true;
  }
  return st;
}

ccord_status ccord_session_request_profile(ccord_session *s, ccord_id user_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (user_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid user id");
  if (ccord_store_user_profile(s->store, user_id) != nullptr || was_profile_requested(s, user_id)) {
    return CCORD_OK;
  }
  return ccord_session_request_profile_ex(s, user_id, CCORD_ID_NONE, ccord_clock_ms(), false, err);
}

ccord_status ccord_session_set_relationship(ccord_session *s, ccord_id user_id,
                                            ccord_relationship_kind kind, ccord_error *err)
{
  SESSION_STALE_GUARD(s, err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (user_id == CCORD_ID_NONE || user_id == ccord_store_self_id(s->store)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid relationship target");
  }
  if (kind > CCORD_RELATIONSHIP_PENDING_OUTGOING) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid relationship kind");
  }
  return ccord_transport_set_relationship(s->transport, user_id, kind, err);
}


// ---- own profile edits (PATCH /users/@me, PATCH /users/@me/profile) ----

ccord_status ccord_session_set_profile_rest(ccord_session *s, ccord_profile_rest *rest)
{
  if (!s) return CCORD_ERR_INVALID;
  if (s->profile_rest == rest) return CCORD_OK;
  ccord_profile_rest_destroy(s->profile_rest);
  s->profile_rest = rest;
  return CCORD_OK;
}

typedef struct session_profile_ctx {
  ccord_session *session;
  ccord_profile_edit_cb cb;
  void *ud;
} session_profile_ctx;

// Discord accepted (part of) the edit: the store takes the values it answered with, nothing else.
static void session_apply_profile(ccord_session *s, const ccord_profile_applied *a)
{
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE || a->fields == 0) return;
  ccord_self_profile_update u = {0};
  char avatar[256] = "", banner[256] = "";
  if (a->fields & (1u << CCORD_PROFILE_FIELD_DISPLAY_NAME)) {
    u.set_display_name = true;
    u.display_name = a->display_name;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_AVATAR)) {
    (void)ccord_profile_cdn_url("avatars", self, a->avatar_hash, avatar, sizeof avatar);
    u.set_avatar = true;
    u.avatar_url = avatar;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_BANNER)) {
    (void)ccord_profile_cdn_url("banners", self, a->banner_hash, banner, sizeof banner);
    u.set_banner = true;
    u.banner_url = banner;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_BIO)) {
    u.set_bio = true;
    u.bio = a->bio;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_PRONOUNS)) {
    u.set_pronouns = true;
    u.pronouns = a->pronouns;
  }
  if (a->fields & (1u << CCORD_PROFILE_FIELD_ACCENT)) {
    u.set_accent = true;
    u.accent_color = a->accent_color >= 0 ? (uint32_t)a->accent_color : 0u;
  }
  ccord_error err;
  if (ccord_store_set_self_profile(s->store, &u, &err) != CCORD_OK) {
    CCORD_LOG_W("session: own profile not applied: %s", err.message);
  }
}

static void session_profile_done(void *ud, const ccord_profile_edit_result *res)
{
  session_profile_ctx *ctx = ud;
  if (res->applied.fields != 0) session_apply_profile(ctx->session, &res->applied);
  if (ctx->cb) ctx->cb(ctx->ud, res);
  ccord_free(ctx);
}

ccord_status ccord_session_edit_profile(ccord_session *s, const ccord_profile_edit *edit, ccord_profile_edit_cb cb,
                                        void *ud, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s || !edit) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null argument");
  if (!s->profile_rest) return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "session: profile editing is not available");
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE || !ccord_store_user(s->store, self)) {
    return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: not signed in yet");
  }
  session_profile_ctx *ctx = ccord_calloc(1, sizeof *ctx);
  if (!ctx) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  ctx->session = s;
  ctx->cb = cb;
  ctx->ud = ud;
  ccord_status st = ccord_profile_edit_start(s->profile_rest, edit, session_profile_done, ctx, err);
  if (st != CCORD_OK) ccord_free(ctx);   // nothing was sent and the callback will not run
  return st;
}


// ---- membership screening ----

ccord_status ccord_session_member_verification_fetch(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  ccord_admin_request req;
  ccord_status st = ccord_admin_get_member_verification(&req, guild_id, err);
  if (st != CCORD_OK) return st;
  return ccord_session_admin(s, &req, err);   // the cached form changes only when the answer arrives
}

const ccord_member_verification *ccord_session_member_verification(const ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < s->nverification_cache; i++) {
    // An entry whose first answer could not be copied (out of memory) holds no form yet: version is never null in a real one.
    if (s->verification_cache[i].guild_id == guild_id) {
      return s->verification_cache[i].form.version ? &s->verification_cache[i].form : nullptr;
    }
  }
  return nullptr;
}

ccord_status ccord_session_member_verification_submit(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  const ccord_member_verification *form = ccord_session_member_verification(s, guild_id);
  if (!form) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "session: no cached verification form");
  ccord_admin_request req;
  ccord_status st = ccord_admin_submit_member_verification(&req, guild_id, form, err);
  if (st != CCORD_OK) return st;
  // Nothing changes locally here: Discord's confirmation (the REST answer / GUILD_MEMBER_UPDATE) clears the
  // member's pending flag in the store, which reopens the send gate.
  return ccord_session_admin(s, &req, err);
}

bool ccord_session_self_pending(const ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  ccord_id self = ccord_store_self_id(s->store);
  if (self == CCORD_ID_NONE) return false;
  const ccord_member *m = ccord_store_find_member(s->store, guild_id, self);
  return m && m->pending;
}

ccord_status ccord_session_guild_profile_fetch(ccord_session *s, ccord_id guild_id, ccord_error *err)
{
  ccord_error_clear(err);
  if (!s) return ccord_error_set(err, CCORD_ERR_INVALID, "session: null session");
  if (guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "session: invalid guild id");
  }
  uint64_t now = ccord_clock_ms();
  if (now == 0) now = 1u;
  session_guild_profile_cache *ent = sess_guild_profile_for(s, guild_id);
  if (!ent) return ccord_error_set(err, CCORD_ERR_NOMEM, "session: out of memory");
  if (ent->loading && ent->sent_ms != 0 && now < ent->sent_ms + SESSION_ACTIVITY_SHELF_WAIT_MS) {
    return CCORD_OK;
  }
  if (ent->fetched_ms != 0 && now >= ent->fetched_ms && now < ent->fetched_ms + SESSION_ACTIVITY_SHELF_TTL_MS) {
    return CCORD_OK;
  }
  ccord_admin_request req;
  ccord_status st = ccord_admin_get_guild_profile(&req, guild_id, err);
  if (st != CCORD_OK) return st;
  ent->loading = true;
  ent->sent_ms = now;
  st = ccord_session_admin(s, &req, err);
  if (st != CCORD_OK) {
    ent = sess_guild_profile_for(s, guild_id);
    if (ent) {
      ent->loading = false;
      ent->sent_ms = 0;
    }
  }
  return st;
}

const ccord_guild_profile *ccord_session_guild_profile(const ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return nullptr;
  for (size_t i = 0; i < s->nguild_profiles; i++) {
    if (s->guild_profiles[i].guild_id == guild_id && s->guild_profiles[i].has_profile) {
      return &s->guild_profiles[i].profile;
    }
  }
  return nullptr;
}

bool ccord_session_guild_profile_loading(const ccord_session *s, ccord_id guild_id)
{
  if (!s || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID) return false;
  for (size_t i = 0; i < s->nguild_profiles; i++) {
    if (s->guild_profiles[i].guild_id == guild_id) {
      return s->guild_profiles[i].loading && !s->guild_profiles[i].has_profile;
    }
  }
  return false;
}


