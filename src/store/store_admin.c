// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/store/admin.h>
#include <ccord/store/expressions.h>
#include <ccord/store/roles.h>

#include "store_internal.h"

// One list with its own arena: replacing the list frees the old strings in one go.
typedef struct admin_list {
  ccord_arena arena;
  void *items;
  size_t n, cap;
  bool loaded;
} admin_list;

typedef struct admin_channel_invites {
  ccord_id channel_id;
  admin_list list;
} admin_channel_invites;

typedef struct admin_guild {
  ccord_id guild_id;
  admin_list bans, invites, audit, automod;
  admin_list webhooks, followed, app_perms, welcome, onboarding;
  admin_channel_invites *chan_invites;
  size_t n_chan_invites, cap_chan_invites;
  bool webhooks_stale;
  ccord_id webhooks_stale_channel_id;
  ccord_prune_info prune;
  char last_invite_code[36];
  uint64_t last_invite_seq;
} admin_guild;

struct admin_cache {
  admin_guild *guilds;
  size_t n, cap;
};

static void list_free(admin_list *l)
{
  ccord_arena_free(&l->arena);
  ccord_free(l->items);
  *l = (admin_list){0};
}

static bool list_ensure_cap(admin_list *l, size_t elem)
{
  if (l->n < l->cap) return true;
  size_t ncap = l->cap ? l->cap * 2u : 8u;
  void *grown = ccord_realloc_array(l->items, ncap, elem);
  if (!grown) return false;
  l->items = grown;
  l->cap = ncap;
  return true;
}

static void guild_lists_free(admin_guild *g)
{
  list_free(&g->bans);
  list_free(&g->invites);
  list_free(&g->audit);
  list_free(&g->automod);
  list_free(&g->webhooks);
  list_free(&g->followed);
  list_free(&g->app_perms);
  list_free(&g->welcome);
  list_free(&g->onboarding);
  for (size_t i = 0; i < g->n_chan_invites; i++) {
    list_free(&g->chan_invites[i].list);
  }
  ccord_free(g->chan_invites);
  g->chan_invites = nullptr;
  g->n_chan_invites = 0;
  g->cap_chan_invites = 0;
}

void ccord_store_admin_cache_destroy(struct admin_cache *c)
{
  if (!c) return;
  for (size_t i = 0; i < c->n; i++) guild_lists_free(&c->guilds[i]);
  ccord_free(c->guilds);
  ccord_free(c);
}

static admin_guild *guild_find(const struct admin_cache *c, ccord_id guild_id)
{
  if (!c) return nullptr;
  for (size_t i = 0; i < c->n; i++) {
    if (c->guilds[i].guild_id == guild_id) return &c->guilds[i];
  }
  return nullptr;
}

static admin_guild *guild_get(ccord_store *s, ccord_id guild_id)
{
  struct admin_cache **slot = ccord_store_admin_slot(s);
  if (!slot) return nullptr;
  if (!*slot) *slot = ccord_calloc(1, sizeof **slot);
  struct admin_cache *c = *slot;
  if (!c) return nullptr;
  admin_guild *g = guild_find(c, guild_id);
  if (g) return g;
  if (c->n == c->cap) {
    size_t ncap = c->cap ? c->cap * 2u : 4u;
    admin_guild *grown = ccord_realloc_array(c->guilds, ncap, sizeof *grown);
    if (!grown) return nullptr;
    c->guilds = grown;
    c->cap = ncap;
  }
  c->guilds[c->n] = (admin_guild){.guild_id = guild_id};
  return &c->guilds[c->n++];
}

static const char *dup(ccord_arena *a, const char *s)
{
  return ccord_arena_strndup(a, s ? s : "", s ? strlen(s) : 0);
}

typedef bool (*copy_fn)(ccord_arena *a, void *dst, const void *src);

// Builds a deep copy of `count` elements into a fresh arena and swaps it in. The old list survives a failure.
static ccord_status list_set(admin_list *l, const void *src, size_t count, size_t elem, copy_fn copy, ccord_error *err)
{
  admin_list fresh = {0};
  ccord_arena_init(&fresh.arena, 4096);
  if (count > 0) {
    fresh.items = ccord_alloc_array(count, elem);
    if (!fresh.items) {
      ccord_arena_free(&fresh.arena);
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    for (size_t i = 0; i < count; i++) {
      if (!copy(&fresh.arena, (char *)fresh.items + i * elem, (const char *)src + i * elem)) {
        list_free(&fresh);
        return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
      }
    }
  }
  fresh.n = fresh.cap = count;
  fresh.loaded = true;
  list_free(l);
  *l = fresh;
  return CCORD_OK;
}

static bool copy_ban(ccord_arena *a, void *dst, const void *src)
{
  const ccord_ban *b = src;
  ccord_ban *d = dst;
  *d = *b;
  d->name = dup(a, b->name);
  d->reason = dup(a, b->reason);
  return d->name && d->reason;
}

static bool copy_invite(ccord_arena *a, void *dst, const void *src)
{
  const ccord_invite *v = src;
  ccord_invite *d = dst;
  *d = *v;
  d->code = dup(a, v->code);
  d->channel_name = dup(a, v->channel_name);
  d->inviter_name = dup(a, v->inviter_name);
  return d->code && d->channel_name && d->inviter_name;
}

static bool copy_audit(ccord_arena *a, void *dst, const void *src)
{
  const ccord_audit_entry *v = src;
  ccord_audit_entry *d = dst;
  *d = *v;
  d->user_name = dup(a, v->user_name);
  d->target_name = dup(a, v->target_name);
  d->reason = dup(a, v->reason);
  d->detail = dup(a, v->detail);
  return d->user_name && d->target_name && d->reason && d->detail;
}

static bool copy_str_array(ccord_arena *a, const char *const **dst_out, size_t *dst_count,
                           const char *const *src, size_t count, size_t cap)
{
  if (count > cap) count = cap;
  *dst_count = count;
  if (count == 0) {
    *dst_out = nullptr;
    return true;
  }
  if (!src) return false;
  const char **arr = ccord_arena_alloc(a, count * sizeof *arr, alignof(const char *));
  if (!arr) return false;
  for (size_t i = 0; i < count; i++) {
    arr[i] = dup(a, src[i]);
    if (!arr[i]) return false;
  }
  *dst_out = arr;
  return true;
}

static bool copy_automod(ccord_arena *a, void *dst, const void *src)
{
  const ccord_automod_rule *v = src;
  ccord_automod_rule *d = dst;
  *d = *v;
  if (d->exempt_role_count > CCORD_AUTOMOD_MAX_EXEMPT_ROLES) {
    d->exempt_role_count = CCORD_AUTOMOD_MAX_EXEMPT_ROLES;
  }
  if (d->exempt_channel_count > CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS) {
    d->exempt_channel_count = CCORD_AUTOMOD_MAX_EXEMPT_CHANNELS;
  }
  d->name = dup(a, v->name);
  d->custom_message = dup(a, v->custom_message);
  if (!d->name || !d->custom_message) return false;
  if (!copy_str_array(a, &d->keywords, &d->keyword_count, v->keywords, v->keyword_count, CCORD_AUTOMOD_MAX_KEYWORDS)) {
    return false;
  }
  if (!copy_str_array(a, &d->regex_patterns, &d->regex_count, v->regex_patterns, v->regex_count,
                      CCORD_AUTOMOD_MAX_REGEX)) {
    return false;
  }
  if (!copy_str_array(a, &d->allow_list, &d->allow_count, v->allow_list, v->allow_count,
                      CCORD_AUTOMOD_MAX_ALLOW_LIST)) {
    return false;
  }
  return true;
}

static bool copy_webhook(ccord_arena *a, void *dst, const void *src)
{
  const ccord_webhook *v = src;
  ccord_webhook *d = dst;
  *d = *v;
  d->name = dup(a, v->name);
  d->avatar = dup(a, v->avatar);
  d->token = dup(a, v->token);
  d->creator_name = dup(a, v->creator_name);
  d->source_guild_name = dup(a, v->source_guild_name);
  d->source_channel_name = dup(a, v->source_channel_name);
  return d->name && d->avatar && d->token && d->creator_name && d->source_guild_name && d->source_channel_name;
}

static bool copy_app_perms(ccord_arena *a, void *dst, const void *src)
{
  (void)a;
  const ccord_app_cmd_perms *v = src;
  ccord_app_cmd_perms *d = dst;
  *d = *v;
  if (d->rule_count > CCORD_APP_CMD_MAX_PERMS) d->rule_count = CCORD_APP_CMD_MAX_PERMS;
  return true;
}

static bool copy_welcome_screen(ccord_arena *a, void *dst, const void *src)
{
  const ccord_welcome_screen *v = src;
  ccord_welcome_screen *d = dst;
  *d = *v;
  d->description = dup(a, v->description);
  if (!d->description) return false;
  size_t n = v->channel_count <= CCORD_WELCOME_MAX_CHANNELS ? v->channel_count : CCORD_WELCOME_MAX_CHANNELS;
  d->channel_count = n;
  if (n == 0) {
    d->channels = nullptr;
    return true;
  }
  if (!v->channels) return false;
  ccord_welcome_channel *ch = ccord_arena_alloc(a, n * sizeof *ch, alignof(ccord_welcome_channel));
  if (!ch) return false;
  for (size_t i = 0; i < n; i++) {
    ch[i] = v->channels[i];
    ch[i].description = dup(a, v->channels[i].description);
    ch[i].emoji_name = dup(a, v->channels[i].emoji_name);
    if (!ch[i].description || !ch[i].emoji_name) return false;
  }
  d->channels = ch;
  return true;
}

static bool copy_onboarding(ccord_arena *a, void *dst, const void *src)
{
  const ccord_onboarding *v = src;
  ccord_onboarding *d = dst;
  *d = *v;
  if (d->default_channel_count > CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS) {
    d->default_channel_count = CCORD_ONBOARDING_MAX_DEFAULT_CHANNELS;
  }
  size_t np = v->prompt_count <= CCORD_ONBOARDING_MAX_PROMPTS ? v->prompt_count : CCORD_ONBOARDING_MAX_PROMPTS;
  d->prompt_count = np;
  if (np == 0) {
    d->prompts = nullptr;
    return true;
  }
  if (!v->prompts) return false;
  ccord_onboarding_prompt *pr = ccord_arena_alloc(a, np * sizeof *pr, alignof(ccord_onboarding_prompt));
  if (!pr) return false;
  for (size_t i = 0; i < np; i++) {
    pr[i] = v->prompts[i];
    pr[i].title = dup(a, v->prompts[i].title);
    if (!pr[i].title) return false;
    size_t no = v->prompts[i].option_count <= CCORD_ONBOARDING_MAX_OPTIONS
                  ? v->prompts[i].option_count
                  : CCORD_ONBOARDING_MAX_OPTIONS;
    pr[i].option_count = no;
    if (no == 0) {
      pr[i].options = nullptr;
      continue;
    }
    if (!v->prompts[i].options) return false;
    ccord_onboarding_option *opts = ccord_arena_alloc(a, no * sizeof *opts, alignof(ccord_onboarding_option));
    if (!opts) return false;
    for (size_t j = 0; j < no; j++) {
      opts[j] = v->prompts[i].options[j];
      if (opts[j].channel_count > CCORD_ONBOARDING_MAX_OPT_CHANNELS) {
        opts[j].channel_count = CCORD_ONBOARDING_MAX_OPT_CHANNELS;
      }
      if (opts[j].role_count > CCORD_ONBOARDING_MAX_OPT_ROLES) {
        opts[j].role_count = CCORD_ONBOARDING_MAX_OPT_ROLES;
      }
      opts[j].title = dup(a, v->prompts[i].options[j].title);
      opts[j].description = dup(a, v->prompts[i].options[j].description);
      opts[j].emoji_name = dup(a, v->prompts[i].options[j].emoji_name);
      if (!opts[j].title || !opts[j].description || !opts[j].emoji_name) return false;
    }
    pr[i].options = opts;
  }
  d->prompts = pr;
  return true;
}

static ccord_status check(ccord_store *s, ccord_id guild_id, const void *items, size_t count, ccord_error *err)
{
  if (!s || guild_id == CCORD_ID_NONE || guild_id == CCORD_DM_GUILD_ID || (count > 0 && !items)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad admin list arguments");
  }
  if (!ccord_store_guild(s, guild_id)) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "store: unknown guild");
  return CCORD_OK;
}

// ---- bans ----

ccord_status ccord_store_set_bans(ccord_store *s, ccord_id guild_id, const ccord_ban *bans, size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, bans, count, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&g->bans, bans, count, sizeof *bans, copy_ban, err);
  if (st == CCORD_OK) ccord_store_roles_mutated(s);
  return st;
}

bool ccord_store_remove_ban(ccord_store *s, ccord_id guild_id, ccord_id user_id)
{
  admin_guild *g = s ? guild_find(*ccord_store_admin_slot(s), guild_id) : nullptr;
  if (!g || !g->bans.loaded) return false;
  ccord_ban *items = g->bans.items;
  for (size_t i = 0; i < g->bans.n; i++) {
    if (items[i].user_id != user_id) continue;
    memmove(&items[i], &items[i + 1], (g->bans.n - i - 1) * sizeof *items);
    g->bans.n--;
    ccord_store_roles_mutated(s);
    return true;
  }
  return false;
}

bool ccord_store_bans_loaded(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->bans.loaded;
}

const ccord_ban *ccord_store_bans(const ccord_store *s, ccord_id guild_id, size_t *count)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (count) *count = g && g->bans.loaded ? g->bans.n : 0;
  return g && g->bans.loaded && g->bans.n > 0 ? g->bans.items : nullptr;
}

// ---- invites ----

ccord_status ccord_store_set_invites(ccord_store *s, ccord_id guild_id, const ccord_invite *invites, size_t count,
                                     ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, invites, count, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&g->invites, invites, count, sizeof *invites, copy_invite, err);
  if (st == CCORD_OK) ccord_store_roles_mutated(s);
  return st;
}

static ccord_status list_upsert_invite(admin_list *l, const ccord_invite *invite, bool *mutated, ccord_error *err)
{
  if (!l || !l->loaded) return CCORD_OK;
  ccord_invite *items = l->items;
  for (size_t i = 0; i < l->n; i++) {
    if (strcmp(items[i].code, invite->code) != 0) continue;
    ccord_invite copy;
    if (!copy_invite(&l->arena, &copy, invite)) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    items[i] = copy;   // the old strings stay in the arena until the list is replaced
    *mutated = true;
    return CCORD_OK;
  }
  if (!list_ensure_cap(l, sizeof *items)) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  items = l->items;
  if (!copy_invite(&l->arena, &items[l->n], invite)) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  l->n++;
  *mutated = true;
  return CCORD_OK;
}

static bool list_remove_invite(admin_list *l, const char *code)
{
  if (!l || !l->loaded) return false;
  ccord_invite *items = l->items;
  for (size_t i = 0; i < l->n; i++) {
    if (strcmp(items[i].code, code) != 0) continue;
    memmove(&items[i], &items[i + 1], (l->n - i - 1) * sizeof *items);
    l->n--;
    return true;
  }
  return false;
}

static admin_channel_invites *chan_invites_find(const admin_guild *g, ccord_id channel_id)
{
  if (!g || channel_id == CCORD_ID_NONE) return nullptr;
  for (size_t i = 0; i < g->n_chan_invites; i++) {
    if (g->chan_invites[i].channel_id == channel_id) return &g->chan_invites[i];
  }
  return nullptr;
}

static admin_channel_invites *chan_invites_get(admin_guild *g, ccord_id channel_id)
{
  admin_channel_invites *ci = chan_invites_find(g, channel_id);
  if (ci) return ci;
  if (g->n_chan_invites == g->cap_chan_invites) {
    size_t ncap = g->cap_chan_invites ? g->cap_chan_invites * 2u : 4u;
    admin_channel_invites *grown = ccord_realloc_array(g->chan_invites, ncap, sizeof *grown);
    if (!grown) return nullptr;
    g->chan_invites = grown;
    g->cap_chan_invites = ncap;
  }
  g->chan_invites[g->n_chan_invites] = (admin_channel_invites){.channel_id = channel_id};
  return &g->chan_invites[g->n_chan_invites++];
}

ccord_status ccord_store_upsert_invite(ccord_store *s, ccord_id guild_id, const ccord_invite *invite, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, invite, invite ? 1u : 0u, err);
  if (st != CCORD_OK) return st;
  if (!invite || !invite->code || invite->code[0] == '\0') return ccord_error_set(err, CCORD_ERR_INVALID, "store: invite code");
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  size_t clen = strlen(invite->code);
  if (clen >= sizeof g->last_invite_code) clen = sizeof g->last_invite_code - 1u;
  memcpy(g->last_invite_code, invite->code, clen);
  g->last_invite_code[clen] = '\0';
  g->last_invite_seq++;
  bool mutated = false;
  st = list_upsert_invite(&g->invites, invite, &mutated, err);
  if (st != CCORD_OK) return st;
  if (invite->channel_id != CCORD_ID_NONE) {
    admin_channel_invites *ci = chan_invites_find(g, invite->channel_id);
    if (ci) {
      st = list_upsert_invite(&ci->list, invite, &mutated, err);
      if (st != CCORD_OK) return st;
    }
  }
  if (mutated) ccord_store_roles_mutated(s);
  return CCORD_OK;
}

bool ccord_store_remove_invite(ccord_store *s, ccord_id guild_id, const char *code)
{
  admin_guild *g = s && code ? guild_find(*ccord_store_admin_slot(s), guild_id) : nullptr;
  if (!g) return false;
  bool removed = list_remove_invite(&g->invites, code);
  for (size_t i = 0; i < g->n_chan_invites; i++) {
    if (list_remove_invite(&g->chan_invites[i].list, code)) removed = true;
  }
  if (removed) ccord_store_roles_mutated(s);
  return removed;
}

bool ccord_store_invites_loaded(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->invites.loaded;
}

const ccord_invite *ccord_store_invites(const ccord_store *s, ccord_id guild_id, size_t *count)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (count) *count = g && g->invites.loaded ? g->invites.n : 0;
  return g && g->invites.loaded && g->invites.n > 0 ? g->invites.items : nullptr;
}

bool ccord_store_last_invite(const ccord_store *s, ccord_id guild_id, char out_code[static 36], uint64_t *out_seq)
{
  out_code[0] = '\0';
  if (out_seq) *out_seq = 0;
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (!g || g->last_invite_seq == 0 || g->last_invite_code[0] == '\0') return false;
  memcpy(out_code, g->last_invite_code, sizeof g->last_invite_code);
  if (out_seq) *out_seq = g->last_invite_seq;
  return true;
}

ccord_status ccord_store_set_channel_invites(ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                             const ccord_invite *invites, size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  if (channel_id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad channel id");
  ccord_status st = check(s, guild_id, invites, count, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  admin_channel_invites *ci = chan_invites_get(g, channel_id);
  if (!ci) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&ci->list, invites, count, sizeof *invites, copy_invite, err);
  if (st == CCORD_OK) ccord_store_roles_mutated(s);
  return st;
}

bool ccord_store_channel_invites_loaded(const ccord_store *s, ccord_id guild_id, ccord_id channel_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  const admin_channel_invites *ci = chan_invites_find(g, channel_id);
  return ci && ci->list.loaded;
}

const ccord_invite *ccord_store_channel_invites(const ccord_store *s, ccord_id guild_id, ccord_id channel_id,
                                                size_t *count)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  const admin_channel_invites *ci = chan_invites_find(g, channel_id);
  if (count) *count = ci && ci->list.loaded ? ci->list.n : 0;
  return ci && ci->list.loaded && ci->list.n > 0 ? ci->list.items : nullptr;
}

void ccord_store_forget_channel_invites(ccord_store *s, ccord_id guild_id, ccord_id channel_id)
{
  if (!s || channel_id == CCORD_ID_NONE) return;
  struct admin_cache **slot = ccord_store_admin_slot(s);
  admin_guild *g = slot ? guild_find(*slot, guild_id) : nullptr;
  if (!g) return;
  for (size_t i = 0; i < g->n_chan_invites; i++) {
    if (g->chan_invites[i].channel_id != channel_id) continue;
    list_free(&g->chan_invites[i].list);
    memmove(&g->chan_invites[i], &g->chan_invites[i + 1], (g->n_chan_invites - i - 1) * sizeof *g->chan_invites);
    g->n_chan_invites--;
    ccord_store_roles_mutated(s);
    return;
  }
}

// ---- audit log ----

ccord_status ccord_store_set_audit(ccord_store *s, ccord_id guild_id, const ccord_audit_entry *entries, size_t count,
                                   ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, entries, count, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&g->audit, entries, count, sizeof *entries, copy_audit, err);
  if (st == CCORD_OK) ccord_store_roles_mutated(s);
  return st;
}

bool ccord_store_audit_loaded(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->audit.loaded;
}

const ccord_audit_entry *ccord_store_audit(const ccord_store *s, ccord_id guild_id, size_t *count)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (count) *count = g && g->audit.loaded ? g->audit.n : 0;
  return g && g->audit.loaded && g->audit.n > 0 ? g->audit.items : nullptr;
}

// ---- prune ----

ccord_status ccord_store_set_prune_info(ccord_store *s, ccord_id guild_id, const ccord_prune_info *info,
                                        ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, info, 1u, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  g->prune = *info;
  g->prune.has_result = true;
  ccord_store_roles_mutated(s);
  return CCORD_OK;
}

bool ccord_store_prune_info(const ccord_store *s, ccord_id guild_id, ccord_prune_info *out)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (!g || !g->prune.has_result) {
    if (out) *out = (ccord_prune_info){0};
    return false;
  }
  if (out) *out = g->prune;
  return true;
}

void ccord_store_clear_prune_info(ccord_store *s, ccord_id guild_id)
{
  admin_guild *g = s ? guild_find(*ccord_store_admin_slot(s), guild_id) : nullptr;
  if (!g || !g->prune.has_result) return;
  g->prune = (ccord_prune_info){0};
  ccord_store_roles_mutated(s);
}

// ---- automod rules ----

ccord_status ccord_store_set_automod_rules(ccord_store *s, ccord_id guild_id, const ccord_automod_rule *rules,
                                           size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, rules, count, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&g->automod, rules, count, sizeof *rules, copy_automod, err);
  if (st == CCORD_OK) ccord_store_roles_mutated(s);
  return st;
}

ccord_status ccord_store_upsert_automod_rule(ccord_store *s, ccord_id guild_id, const ccord_automod_rule *rule,
                                             ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, rule, rule ? 1u : 0u, err);
  if (st != CCORD_OK) return st;
  if (!rule || rule->id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: automod rule id");
  admin_guild *g = guild_find(*ccord_store_admin_slot(s), guild_id);
  if (!g || !g->automod.loaded) return CCORD_OK;   // the list answer will bring it in
  ccord_automod_rule *items = g->automod.items;
  for (size_t i = 0; i < g->automod.n; i++) {
    if (items[i].id != rule->id) continue;
    ccord_automod_rule copy;
    if (!copy_automod(&g->automod.arena, &copy, rule)) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    items[i] = copy;
    ccord_store_roles_mutated(s);
    return CCORD_OK;
  }
  if (!list_ensure_cap(&g->automod, sizeof *items)) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  items = g->automod.items;
  if (!copy_automod(&g->automod.arena, &items[g->automod.n], rule)) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  g->automod.n++;
  ccord_store_roles_mutated(s);
  return CCORD_OK;
}

bool ccord_store_remove_automod_rule(ccord_store *s, ccord_id guild_id, ccord_id rule_id)
{
  admin_guild *g = s && rule_id != CCORD_ID_NONE ? guild_find(*ccord_store_admin_slot(s), guild_id) : nullptr;
  if (!g || !g->automod.loaded) return false;
  ccord_automod_rule *items = g->automod.items;
  for (size_t i = 0; i < g->automod.n; i++) {
    if (items[i].id != rule_id) continue;
    memmove(&items[i], &items[i + 1], (g->automod.n - i - 1) * sizeof *items);
    g->automod.n--;
    ccord_store_roles_mutated(s);
    return true;
  }
  return false;
}

bool ccord_store_automod_loaded(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->automod.loaded;
}

const ccord_automod_rule *ccord_store_automod_rules(const ccord_store *s, ccord_id guild_id, size_t *count)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (count) *count = g && g->automod.loaded ? g->automod.n : 0;
  return g && g->automod.loaded && g->automod.n > 0 ? g->automod.items : nullptr;
}

// ---- integrations: webhooks, followed channels, app command permissions ----

ccord_status ccord_store_set_webhooks(ccord_store *s, ccord_id guild_id, const ccord_webhook *items, size_t count,
                                      ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, items, count, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&g->webhooks, items, count, sizeof *items, copy_webhook, err);
  if (st == CCORD_OK) {
    g->webhooks_stale = false;
    g->webhooks_stale_channel_id = CCORD_ID_NONE;
    ccord_store_roles_mutated(s);
  }
  return st;
}

ccord_status ccord_store_upsert_webhook(ccord_store *s, ccord_id guild_id, const ccord_webhook *item, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, item, item ? 1u : 0u, err);
  if (st != CCORD_OK) return st;
  if (!item || item->id == CCORD_ID_NONE) return ccord_error_set(err, CCORD_ERR_INVALID, "store: webhook id");
  admin_guild *g = guild_find(*ccord_store_admin_slot(s), guild_id);
  if (!g || !g->webhooks.loaded) return CCORD_OK;
  ccord_webhook *items = g->webhooks.items;
  for (size_t i = 0; i < g->webhooks.n; i++) {
    if (items[i].id != item->id) continue;
    ccord_webhook merged = *item;
    if ((!merged.token || merged.token[0] == '\0') && items[i].token && items[i].token[0] != '\0') {
      merged.token = items[i].token;
    }
    ccord_webhook copy;
    if (!copy_webhook(&g->webhooks.arena, &copy, &merged)) {
      return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
    }
    items[i] = copy;
    ccord_store_roles_mutated(s);
    return CCORD_OK;
  }
  if (!list_ensure_cap(&g->webhooks, sizeof *items)) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  items = g->webhooks.items;
  if (!copy_webhook(&g->webhooks.arena, &items[g->webhooks.n], item)) {
    return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  }
  g->webhooks.n++;
  ccord_store_roles_mutated(s);
  return CCORD_OK;
}

bool ccord_store_remove_webhook(ccord_store *s, ccord_id guild_id, ccord_id webhook_id)
{
  admin_guild *g = s && webhook_id != CCORD_ID_NONE ? guild_find(*ccord_store_admin_slot(s), guild_id) : nullptr;
  if (!g || !g->webhooks.loaded) return false;
  ccord_webhook *items = g->webhooks.items;
  for (size_t i = 0; i < g->webhooks.n; i++) {
    if (items[i].id != webhook_id) continue;
    memmove(&items[i], &items[i + 1], (g->webhooks.n - i - 1) * sizeof *items);
    g->webhooks.n--;
    ccord_store_roles_mutated(s);
    return true;
  }
  return false;
}

bool ccord_store_webhooks_loaded(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->webhooks.loaded;
}

const ccord_webhook *ccord_store_webhooks(const ccord_store *s, ccord_id guild_id, size_t *count)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (count) *count = g && g->webhooks.loaded ? g->webhooks.n : 0;
  return g && g->webhooks.loaded && g->webhooks.n > 0 ? g->webhooks.items : nullptr;
}

void ccord_store_note_webhooks_update(ccord_store *s, ccord_id guild_id, ccord_id channel_id)
{
  admin_guild *g = s && guild_id != CCORD_ID_NONE ? guild_find(*ccord_store_admin_slot(s), guild_id) : nullptr;
  if (!g || !g->webhooks.loaded) return;
  g->webhooks_stale = true;
  g->webhooks_stale_channel_id = channel_id;
  ccord_store_roles_mutated(s);
}

bool ccord_store_webhooks_stale(const ccord_store *s, ccord_id guild_id, ccord_id *out_channel_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (!g || !g->webhooks.loaded || !g->webhooks_stale) {
    if (out_channel_id) *out_channel_id = CCORD_ID_NONE;
    return false;
  }
  if (out_channel_id) *out_channel_id = g->webhooks_stale_channel_id;
  return true;
}

ccord_status ccord_store_add_followed_channel(ccord_store *s, ccord_id guild_id, const ccord_followed_channel *fc,
                                              ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, fc, fc ? 1u : 0u, err);
  if (st != CCORD_OK) return st;
  if (!fc || fc->source_channel_id == CCORD_ID_NONE || fc->target_channel_id == CCORD_ID_NONE ||
      fc->webhook_id == CCORD_ID_NONE) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad followed channel");
  }
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  ccord_followed_channel *items = g->followed.items;
  for (size_t i = 0; i < g->followed.n; i++) {
    if (items[i].webhook_id == fc->webhook_id) {
      items[i] = *fc;
      ccord_store_roles_mutated(s);
      return CCORD_OK;
    }
  }
  if (!list_ensure_cap(&g->followed, sizeof *items)) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  items = g->followed.items;
  items[g->followed.n++] = *fc;
  g->followed.loaded = true;
  ccord_store_roles_mutated(s);
  return CCORD_OK;
}

const ccord_followed_channel *ccord_store_followed_channels(const ccord_store *s, ccord_id guild_id, size_t *count)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (count) *count = g ? g->followed.n : 0;
  return g && g->followed.n > 0 ? g->followed.items : nullptr;
}

ccord_status ccord_store_set_app_cmd_perms(ccord_store *s, ccord_id guild_id, const ccord_app_cmd_perms *items,
                                           size_t count, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_status st = check(s, guild_id, items, count, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&g->app_perms, items, count, sizeof *items, copy_app_perms, err);
  if (st == CCORD_OK) ccord_store_roles_mutated(s);
  return st;
}

bool ccord_store_app_cmd_perms_loaded(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->app_perms.loaded;
}

const ccord_app_cmd_perms *ccord_store_app_cmd_perms(const ccord_store *s, ccord_id guild_id, size_t *count)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  if (count) *count = g && g->app_perms.loaded ? g->app_perms.n : 0;
  return g && g->app_perms.loaded && g->app_perms.n > 0 ? g->app_perms.items : nullptr;
}

// ---- community: welcome screen & server onboarding ----

ccord_status ccord_store_set_welcome_screen(ccord_store *s, ccord_id guild_id, const ccord_welcome_screen *ws,
                                            ccord_error *err)
{
  ccord_error_clear(err);
  if (!ws) return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad admin list arguments");
  ccord_status st = check(s, guild_id, ws, 1u, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&g->welcome, ws, 1u, sizeof *ws, copy_welcome_screen, err);
  if (st == CCORD_OK) ccord_store_roles_mutated(s);
  return st;
}

bool ccord_store_welcome_screen_loaded(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->welcome.loaded;
}

const ccord_welcome_screen *ccord_store_welcome_screen(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->welcome.loaded && g->welcome.n > 0 ? g->welcome.items : nullptr;
}

ccord_status ccord_store_set_onboarding(ccord_store *s, ccord_id guild_id, const ccord_onboarding *ob, ccord_error *err)
{
  ccord_error_clear(err);
  if (!ob) return ccord_error_set(err, CCORD_ERR_INVALID, "store: bad admin list arguments");
  ccord_status st = check(s, guild_id, ob, 1u, err);
  if (st != CCORD_OK) return st;
  admin_guild *g = guild_get(s, guild_id);
  if (!g) return ccord_error_set(err, CCORD_ERR_NOMEM, "store: out of memory");
  st = list_set(&g->onboarding, ob, 1u, sizeof *ob, copy_onboarding, err);
  if (st == CCORD_OK) ccord_store_roles_mutated(s);
  return st;
}

bool ccord_store_onboarding_loaded(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->onboarding.loaded;
}

const ccord_onboarding *ccord_store_onboarding(const ccord_store *s, ccord_id guild_id)
{
  const admin_guild *g = s ? guild_find(ccord_store_admin_slot_const(s), guild_id) : nullptr;
  return g && g->onboarding.loaded && g->onboarding.n > 0 ? g->onboarding.items : nullptr;
}

void ccord_store_forget_admin_lists(ccord_store *s, ccord_id guild_id)
{
  if (s) ccord_store_forget_guild_expression_admin(s, guild_id);
  struct admin_cache **slot = s ? ccord_store_admin_slot(s) : nullptr;
  admin_guild *g = slot ? guild_find(*slot, guild_id) : nullptr;
  if (!g) return;
  guild_lists_free(g);
  struct admin_cache *c = *slot;
  size_t i = (size_t)(g - c->guilds);
  memmove(&c->guilds[i], &c->guilds[i + 1], (c->n - i - 1) * sizeof c->guilds[0]);
  c->n--;
  ccord_store_roles_mutated(s);
}

void ccord_store_admin_reset(ccord_store *s)
{
  struct admin_cache **slot = s ? ccord_store_admin_slot(s) : nullptr;
  if (!slot || !*slot) return;
  ccord_store_admin_cache_destroy(*slot);
  *slot = nullptr;
}
