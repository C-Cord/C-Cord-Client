// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/net/governor.h>

#include <math.h>
#include <string.h>

#include <ccord/core/alloc.h>

#define GOV_KEY_MAX 160u
#define GOV_MAJOR_MAX 24u
#define GOV_ROUTES 192u
#define GOV_BUCKETS 192u
#define GOV_RING 512u
#define GOV_POLL_HINT_MS 20u
#define GOV_EDGE_MAX_MS 3600000u

typedef struct gov_bucket {
  bool used;
  uint32_t gen;
  char key[GOV_KEY_MAX];
  int32_t limit;
  int32_t remaining;  // -1 unknown
  uint64_t reset_at_ms;
  uint64_t blocked_until_ms;
  uint64_t last_used_ms;
  uint32_t inflight;
} gov_bucket;

typedef struct gov_route {
  bool used;
  uint32_t gen;
  char key[GOV_KEY_MAX];
  char major[GOV_MAJOR_MAX];
  int32_t bucket;  // index into buckets, -1 when none
  uint32_t bucket_gen;
  uint64_t last_used_ms;
  uint32_t inflight;
  uint32_t requests;
  uint32_t rate_limited;
  int last_status;
} gov_route;

struct ccord_gov {
  ccord_gov_config cfg;
  gov_bucket buckets[GOV_BUCKETS];
  gov_route routes[GOV_ROUTES];
  uint32_t inflight;
  double tokens;
  uint64_t tokens_at_ms;
  bool tokens_ready;
  uint64_t global_until_ms;
  uint64_t backoff_until_ms;
  uint64_t edge_until_ms;
  uint32_t consecutive_failures;
  uint32_t consecutive_429;
  uint32_t edge_strikes;
  bool auth_failed;
  uint64_t invalid_ring[GOV_RING];
  uint32_t invalid_len;   // number of valid entries (<= GOV_RING)
  uint32_t invalid_head;  // next write slot
  uint64_t rng;
  ccord_gov_stats stats;
};

// ---------------------------------------------------------------- route keys

static bool seg_is(const char *s, size_t n, const char *lit)
{
  size_t l = strlen(lit);
  return l == n && memcmp(s, lit, n) == 0;
}

static bool all_digits(const char *s, size_t n)
{
  if (n == 0) return false;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

bool ccord_gov_route_key(const char *method, const char *path, char *route, size_t route_cap, char *major,
                         size_t major_cap)
{
  if (!method || !path || path[0] != '/' || !route || route_cap < 8u || !major || major_cap < 2u) return false;
  size_t ml = strlen(method);
  if (ml == 0 || ml + 2u > route_cap) return false;
  memcpy(route, method, ml);
  size_t o = ml;
  route[o++] = ' ';
  major[0] = '0';
  major[1] = '\0';
  bool have_major = false;
  char prev[16] = "";
  const char *p = path;
  size_t segments = 0;
  while (*p == '/') {
    p++;
    const char *start = p;
    while (*p != '\0' && *p != '/' && *p != '?' && *p != '#') p++;
    size_t n = (size_t)(p - start);
    if (n == 0) continue;
    const char *emit = start;
    size_t emit_n = n;
    if (all_digits(start, n)) {
      bool is_channels = seg_is(prev, strlen(prev), "channels");
      bool is_guilds = seg_is(prev, strlen(prev), "guilds");
      bool major_parent = is_channels || is_guilds || seg_is(prev, strlen(prev), "webhooks");
      if (is_guilds && strncmp(path, "/users/@me/guilds/", 18) == 0) {
        // When path begins with /users/@me/guilds/, do not treat segment after guilds as major parameter
        major_parent = false;
      }
      if (!have_major && major_parent) {
        if (n >= major_cap) return false;
        memcpy(major, start, n);
        major[n] = '\0';
        have_major = true;
      } else {
        emit = ":id";
        emit_n = 3;
      }
    } else if ((seg_is(prev, strlen(prev), "reactions") || seg_is(prev, strlen(prev), "emoji")) && !seg_is(start, n, "@me")) {
      emit = ":emoji";
      emit_n = 6;
    } else if (seg_is(prev, strlen(prev), "invites")) {
      emit = ":code";
      emit_n = 5;
    }
    if (o + 1u + emit_n >= route_cap) return false;
    route[o++] = '/';
    memcpy(route + o, emit, emit_n);
    o += emit_n;
    segments++;
    size_t pn = n < sizeof prev - 1u ? n : sizeof prev - 1u;
    memcpy(prev, start, pn);
    prev[pn] = '\0';
  }
  if (segments == 0) {
    if (o + 1u >= route_cap) return false;
    route[o++] = '/';
  }
  route[o] = '\0';
  return o < GOV_KEY_MAX;
}

// ---------------------------------------------------------------- helpers

static uint64_t gov_rand(ccord_gov *g)
{
  uint64_t x = g->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  g->rng = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static double gov_rand01(ccord_gov *g)
{
  return (double)(gov_rand(g) >> 11) * (1.0 / 9007199254740992.0);
}

// Positive jitter only: a wait is never shortened.
static uint64_t gov_jitter(ccord_gov *g, uint64_t delay_ms)
{
  if (g->cfg.jitter_pct == 0 || delay_ms == 0) return delay_ms;
  double extra = (double)delay_ms * ((double)g->cfg.jitter_pct / 100.0) * gov_rand01(g);
  return delay_ms + (uint64_t)extra;
}

static uint64_t gov_backoff_ms(ccord_gov *g, uint32_t failures)
{
  if (failures == 0) return 0;
  uint32_t shift = failures - 1u;
  if (shift > 16u) shift = 16u;
  uint64_t d = g->cfg.base_backoff_ms << shift;
  if (d > g->cfg.max_backoff_ms || d < g->cfg.base_backoff_ms) d = g->cfg.max_backoff_ms;
  return gov_jitter(g, d);
}

static uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

// Bounded copy that always NUL-terminates (keys are already length-checked; this just keeps the compiler honest).
static void copy_str(char *dst, size_t cap, const char *src)
{
  size_t n = strlen(src);
  if (n >= cap) n = cap - 1u;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void gov_refill(ccord_gov *g, uint64_t now_ms)
{
  if (!g->tokens_ready) {
    g->tokens = g->cfg.token_capacity;
    g->tokens_at_ms = now_ms;
    g->tokens_ready = true;
    return;
  }
  if (now_ms > g->tokens_at_ms) {
    g->tokens += (double)(now_ms - g->tokens_at_ms) / 1000.0 * g->cfg.tokens_per_sec;
    if (g->tokens > g->cfg.token_capacity) g->tokens = g->cfg.token_capacity;
    g->tokens_at_ms = now_ms;
  }
}

static void gov_trip_edge(ccord_gov *g, uint64_t now_ms)
{
  g->edge_strikes++;
  uint32_t shift = g->edge_strikes - 1u;
  if (shift > 6u) shift = 6u;
  uint64_t d = g->cfg.edge_block_ms << shift;
  if (d > GOV_EDGE_MAX_MS) d = GOV_EDGE_MAX_MS;
  g->edge_until_ms = max_u64(g->edge_until_ms, now_ms + gov_jitter(g, d));
  g->invalid_len = 0;
  g->invalid_head = 0;
  g->stats.edge_trips++;
}

static void gov_note_invalid(ccord_gov *g, uint64_t now_ms)
{
  g->invalid_ring[g->invalid_head] = now_ms;
  g->invalid_head = (g->invalid_head + 1u) % GOV_RING;
  if (g->invalid_len < GOV_RING) g->invalid_len++;
  uint32_t limit = g->cfg.invalid_limit > GOV_RING ? GOV_RING : g->cfg.invalid_limit;
  uint32_t in_window = 0;
  for (uint32_t i = 0; i < g->invalid_len; i++) {
    uint64_t t = g->invalid_ring[i];
    if (now_ms >= t && now_ms - t < g->cfg.invalid_window_ms) in_window++;
  }
  if (in_window >= limit) gov_trip_edge(g, now_ms);
}

// ---------------------------------------------------------------- tables

static int32_t gov_find_route(ccord_gov *g, const char *key)
{
  for (uint32_t i = 0; i < GOV_ROUTES; i++) {
    if (g->routes[i].used && strcmp(g->routes[i].key, key) == 0) return (int32_t)i;
  }
  return -1;
}

static int32_t gov_new_route(ccord_gov *g, const char *key, const char *major, uint64_t now_ms)
{
  int32_t slot = -1;
  uint64_t oldest = UINT64_MAX;
  for (uint32_t i = 0; i < GOV_ROUTES; i++) {
    if (!g->routes[i].used) {
      slot = (int32_t)i;
      break;
    }
    if (g->routes[i].inflight == 0 && g->routes[i].last_used_ms < oldest) {
      oldest = g->routes[i].last_used_ms;
      slot = (int32_t)i;
    }
  }
  if (slot < 0) {  // every route busy: evict the least recently used regardless
    oldest = UINT64_MAX;
    for (uint32_t i = 0; i < GOV_ROUTES; i++) {
      if (g->routes[i].last_used_ms < oldest) {
        oldest = g->routes[i].last_used_ms;
        slot = (int32_t)i;
      }
    }
  }
  gov_route *r = &g->routes[slot];
  uint32_t gen = r->gen + 1u;
  memset(r, 0, sizeof *r);
  r->gen = gen;
  r->used = true;
  r->bucket = -1;
  r->last_used_ms = now_ms;
  copy_str(r->key, sizeof r->key, key);
  copy_str(r->major, sizeof r->major, major);
  return slot;
}

static int32_t gov_find_bucket(ccord_gov *g, const char *key)
{
  for (uint32_t i = 0; i < GOV_BUCKETS; i++) {
    if (g->buckets[i].used && strcmp(g->buckets[i].key, key) == 0) return (int32_t)i;
  }
  return -1;
}

static int32_t gov_get_bucket(ccord_gov *g, const char *key, uint64_t now_ms)
{
  int32_t found = gov_find_bucket(g, key);
  if (found >= 0) return found;
  int32_t slot = -1;
  uint64_t oldest = UINT64_MAX;
  for (uint32_t i = 0; i < GOV_BUCKETS; i++) {
    if (!g->buckets[i].used) {
      slot = (int32_t)i;
      break;
    }
    if (g->buckets[i].inflight == 0 && g->buckets[i].last_used_ms < oldest) {
      oldest = g->buckets[i].last_used_ms;
      slot = (int32_t)i;
    }
  }
  if (slot < 0) {
    oldest = UINT64_MAX;
    for (uint32_t i = 0; i < GOV_BUCKETS; i++) {
      if (g->buckets[i].last_used_ms < oldest) {
        oldest = g->buckets[i].last_used_ms;
        slot = (int32_t)i;
      }
    }
  }
  gov_bucket *b = &g->buckets[slot];
  uint32_t gen = b->gen + 1u;
  memset(b, 0, sizeof *b);
  b->gen = gen;
  b->used = true;
  b->remaining = -1;
  b->last_used_ms = now_ms;
  copy_str(b->key, sizeof b->key, key);
  return slot;
}

static gov_bucket *route_bucket(ccord_gov *g, gov_route *r)
{
  if (r->bucket < 0) return nullptr;
  gov_bucket *b = &g->buckets[r->bucket];
  if (!b->used || b->gen != r->bucket_gen) {
    r->bucket = -1;
    return nullptr;
  }
  return b;
}

static void route_attach(ccord_gov *g, gov_route *r, int32_t bucket)
{
  r->bucket = bucket;
  r->bucket_gen = g->buckets[bucket].gen;
}


// ---------------------------------------------------------------- response parsing

#define GOV_MAX_WAIT_MS 3600000u

static bool ci_prefix(const char *s, size_t n, const char *lit)
{
  size_t l = strlen(lit);
  if (n < l) return false;
  for (size_t i = 0; i < l; i++) {
    char a = s[i], b = lit[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

// "12", "0.847", "64.57" (seconds) -> milliseconds, clamped. False when not a plain decimal number.
static bool parse_seconds_ms(const char *s, size_t n, uint64_t *out)
{
  size_t i = 0;
  uint64_t whole = 0;
  bool digits = false;
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    digits = true;
    whole = whole * 10u + (uint64_t)(s[i] - '0');
    if (whole > GOV_MAX_WAIT_MS) whole = GOV_MAX_WAIT_MS;
    i++;
  }
  uint64_t frac = 0, scale = 1;
  if (i < n && s[i] == '.') {
    i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (scale < 1000u) {
        frac = frac * 10u + (uint64_t)(s[i] - '0');
        scale *= 10u;
      }
      i++;
      digits = true;
    }
  }
  if (!digits || i != n) return false;
  while (scale < 1000u) {
    frac *= 10u;
    scale *= 10u;
  }
  uint64_t ms = whole * 1000u + frac;
  *out = ms > GOV_MAX_WAIT_MS ? GOV_MAX_WAIT_MS : ms;
  return true;
}

static bool parse_int(const char *s, size_t n, int32_t *out)
{
  if (n == 0 || n > 9) return false;
  int32_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    v = v * 10 + (s[i] - '0');
  }
  *out = v;
  return true;
}

void ccord_gov_response_init(ccord_gov_response *r, int status)
{
  if (!r) return;
  memset(r, 0, sizeof *r);
  r->status = status;
  r->remaining = -1;
}

void ccord_gov_response_header(ccord_gov_response *r, const char *line, size_t len)
{
  if (!r || !line) return;
  const char *colon = memchr(line, ':', len);
  if (!colon) return;
  size_t name_n = (size_t)(colon - line);
  const char *v = colon + 1;
  size_t vn = len - name_n - 1u;
  while (vn > 0 && (*v == ' ' || *v == '\t')) {
    v++;
    vn--;
  }
  while (vn > 0 && (v[vn - 1] == '\r' || v[vn - 1] == '\n' || v[vn - 1] == ' ' || v[vn - 1] == '\t')) vn--;

  struct {
    const char *name;
    int id;
  } const names[] = {{"x-ratelimit-bucket", 1}, {"x-ratelimit-limit", 2}, {"x-ratelimit-remaining", 3},
                     {"x-ratelimit-reset-after", 4}, {"x-ratelimit-global", 5}, {"retry-after", 6},
                     {"content-type", 7}};
  int id = 0;
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    size_t l = strlen(names[i].name);
    if (l == name_n && ci_prefix(line, name_n, names[i].name)) {
      id = names[i].id;
      break;
    }
  }
  uint64_t ms = 0;
  int32_t iv = 0;
  switch (id) {
    case 1:
      if (vn > 0 && vn < sizeof r->bucket) {
        memcpy(r->bucket, v, vn);
        r->bucket[vn] = '\0';
      }
      break;
    case 2:
      if (parse_int(v, vn, &iv)) r->limit = iv;
      break;
    case 3:
      if (parse_int(v, vn, &iv)) r->remaining = iv;
      break;
    case 4:
      if (parse_seconds_ms(v, vn, &ms)) r->reset_after_ms = ms;
      break;
    case 5:
      r->global = ci_prefix(v, vn, "true");
      break;
    case 6:
      if (parse_seconds_ms(v, vn, &ms)) r->retry_after_ms = ms;
      break;
    case 7:
      if (ci_prefix(v, vn, "text/html")) r->edge_html = true;
      break;
    default:
      break;
  }
}

void ccord_gov_response_body(ccord_gov_response *r, const char *body, size_t len)
{
  if (!r || !body || len == 0) return;
  size_t i = 0;
  while (i < len && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) i++;
  if (i < len && body[i] == '<') r->edge_html = true;
  if (r->edge_html) return;
  const char *key = "\"retry_after\"";
  size_t kn = strlen(key);
  for (size_t k = 0; r->retry_after_ms == 0 && k + kn <= len; k++) {
    if (memcmp(body + k, key, kn) != 0) continue;
    size_t j = k + kn;
    while (j < len && (body[j] == ' ' || body[j] == ':' || body[j] == '\t')) j++;
    size_t s = j;
    while (j < len && ((body[j] >= '0' && body[j] <= '9') || body[j] == '.')) j++;
    uint64_t ms = 0;
    if (j > s && parse_seconds_ms(body + s, j - s, &ms)) r->retry_after_ms = ms;
    break;
  }
  const char *gk = "\"global\"";
  size_t gn = strlen(gk);
  for (size_t k = 0; k + gn <= len; k++) {
    if (memcmp(body + k, gk, gn) != 0) continue;
    size_t j = k + gn;
    while (j < len && (body[j] == ' ' || body[j] == ':' || body[j] == '\t')) j++;
    if (j + 4 <= len && memcmp(body + j, "true", 4) == 0) r->global = true;
    break;
  }
}

// ---------------------------------------------------------------- public API

static void apply_defaults(ccord_gov_config *c)
{
  if (c->max_inflight == 0) c->max_inflight = 4;
  if (c->user_extra_inflight == 0) c->user_extra_inflight = 2;
  if (c->tokens_per_sec <= 0.0) c->tokens_per_sec = 8.0;
  if (c->token_capacity <= 0.0) c->token_capacity = 24.0;
  if (c->visible_reserve < 0.0) c->visible_reserve = 2.0;
  if (c->background_reserve < 0.0) c->background_reserve = 8.0;
  if (c->base_backoff_ms == 0) c->base_backoff_ms = 1000;
  if (c->max_backoff_ms == 0) c->max_backoff_ms = 60000;
  if (c->jitter_pct == 0) c->jitter_pct = 20;
  if (c->jitter_pct > 100) c->jitter_pct = 100;
  if (c->invalid_limit == 0) c->invalid_limit = 100;
  if (c->invalid_window_ms == 0) c->invalid_window_ms = 600000;
  if (c->edge_block_ms == 0) c->edge_block_ms = 300000;
  if (c->seed == 0) c->seed = 0x9E3779B97F4A7C15ULL;
}

ccord_gov_config ccord_gov_config_default(void)
{
  ccord_gov_config c = {.visible_reserve = -1.0, .background_reserve = -1.0};
  apply_defaults(&c);
  return c;
}

ccord_gov *ccord_gov_create(const ccord_gov_config *cfg)
{
  ccord_gov *g = ccord_calloc(1, sizeof *g);
  if (!g) return nullptr;
  g->cfg = cfg ? *cfg : ccord_gov_config_default();
  apply_defaults(&g->cfg);
  g->rng = g->cfg.seed;
  return g;
}

void ccord_gov_destroy(ccord_gov *g) { ccord_free(g); }

static ccord_gov_decision decide(ccord_gov_verdict v, ccord_gov_reason r, uint64_t at)
{
  return (ccord_gov_decision){.verdict = v, .reason = r, .retry_at_ms = at};
}

static ccord_gov_decision gov_acquire(ccord_gov *g, const char *method, const char *path, ccord_gov_priority prio,
                                      uint64_t now_ms, ccord_gov_ticket *ticket, bool pre_auth)
{
  if (ticket) memset(ticket, 0, sizeof *ticket);
  if (!g || !method || !path || !ticket || prio >= CCORD_GOV_PRIORITY_COUNT) {
    return decide(CCORD_GOV_REFUSE, CCORD_GOV_REASON_NONE, 0);
  }
  if (!pre_auth && g->auth_failed) {
    g->stats.refusals++;
    return decide(CCORD_GOV_REFUSE, CCORD_GOV_REASON_AUTH, 0);
  }
  char key[GOV_KEY_MAX], major[GOV_MAJOR_MAX];
  if (!ccord_gov_route_key(method, path, key, sizeof key, major, sizeof major)) {
    return decide(CCORD_GOV_REFUSE, CCORD_GOV_REASON_NONE, 0);
  }

  // An edge block lets exactly one user-action probe through when nothing else is in flight.
  bool probe = false;
  if (now_ms < g->edge_until_ms) {
    if (prio == CCORD_GOV_USER && g->inflight == 0) probe = true;
    else {
      g->stats.waits++;
      return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_EDGE, g->edge_until_ms);
    }
  }
  if (!probe) {
    if (now_ms < g->global_until_ms) {
      g->stats.waits++;
      return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_GLOBAL, g->global_until_ms);
    }
    if (prio != CCORD_GOV_USER && now_ms < g->backoff_until_ms) {
      g->stats.waits++;
      return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_BACKOFF, g->backoff_until_ms);
    }
    uint32_t cap = g->cfg.max_inflight + (prio == CCORD_GOV_USER ? g->cfg.user_extra_inflight : 0u);
    if (g->inflight >= cap) {
      g->stats.waits++;
      return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_INFLIGHT, now_ms + GOV_POLL_HINT_MS);
    }
  }

  int32_t ri = gov_find_route(g, key);
  if (ri < 0) ri = gov_new_route(g, key, major, now_ms);
  gov_route *r = &g->routes[ri];
  gov_bucket *b = route_bucket(g, r);
  if (b) {
    // The window has reset: treat the bucket as refilled so the in-flight check below still limits a burst.
    if (b->remaining == 0 && now_ms >= b->reset_at_ms) b->remaining = b->limit > 0 ? b->limit : 1;
    if (now_ms < b->blocked_until_ms) {
      g->stats.waits++;
      return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_BUCKET, b->blocked_until_ms);
    }
    if (b->remaining == 0 && now_ms < b->reset_at_ms) {
      g->stats.waits++;
      return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_BUCKET, b->reset_at_ms);
    }
    if (b->remaining > 0 && b->inflight >= (uint32_t)b->remaining) {
      g->stats.waits++;
      return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_BUCKET, now_ms + GOV_POLL_HINT_MS);
    }
  } else if (r->inflight >= 2u) {  // bucket not learned yet: be gentle until the first response teaches us
    g->stats.waits++;
    return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_BUCKET, now_ms + GOV_POLL_HINT_MS);
  }

  if (!probe) {
    gov_refill(g, now_ms);
    double need = 1.0;
    if (prio == CCORD_GOV_VISIBLE) need += g->cfg.visible_reserve;
    else if (prio == CCORD_GOV_BACKGROUND) need += g->cfg.background_reserve;
    bool ok = prio == CCORD_GOV_USER ? g->tokens > -g->cfg.token_capacity : g->tokens >= need;
    if (!ok) {
      double missing = (prio == CCORD_GOV_USER ? 1.0 - g->tokens - g->cfg.token_capacity : need - g->tokens);
      uint64_t ms = (uint64_t)ceil(missing / g->cfg.tokens_per_sec * 1000.0);
      g->stats.waits++;
      return decide(CCORD_GOV_WAIT, CCORD_GOV_REASON_TOKENS, now_ms + (ms ? ms : 1u));
    }
  }
  gov_refill(g, now_ms);
  g->tokens -= 1.0;
  if (g->tokens < -g->cfg.token_capacity) g->tokens = -g->cfg.token_capacity;

  g->inflight++;
  r->inflight++;
  r->requests++;
  r->last_used_ms = now_ms;
  if (b) {
    b->inflight++;
    b->last_used_ms = now_ms;
  }
  g->stats.started[prio]++;
  ticket->active = true;
  ticket->prio = prio;
  ticket->route = ri;
  ticket->route_gen = r->gen;
  ticket->bucket = b ? r->bucket : -1;
  ticket->bucket_gen = b ? b->gen : 0;
  return decide(CCORD_GOV_GO, CCORD_GOV_REASON_NONE, 0);
}

ccord_gov_decision ccord_gov_acquire(ccord_gov *g, const char *method, const char *path, ccord_gov_priority prio,
                                     uint64_t now_ms, ccord_gov_ticket *ticket)
{
  return gov_acquire(g, method, path, prio, now_ms, ticket, false);
}

ccord_gov_decision ccord_gov_acquire_preauth(ccord_gov *g, const char *method, const char *path,
                                             ccord_gov_priority prio, uint64_t now_ms, ccord_gov_ticket *ticket)
{
  return gov_acquire(g, method, path, prio, now_ms, ticket, true);
}

ccord_gov_completion ccord_gov_complete(ccord_gov *g, ccord_gov_ticket *ticket, const ccord_gov_response *resp,
                                        uint64_t now_ms)
{
  ccord_gov_completion out = {0};
  if (!g || !ticket || !ticket->active || !resp) return out;
  ticket->active = false;
  if (g->inflight > 0) g->inflight--;

  gov_route *r = nullptr;
  if (ticket->route >= 0 && (uint32_t)ticket->route < GOV_ROUTES && g->routes[ticket->route].used &&
      g->routes[ticket->route].gen == ticket->route_gen) {
    r = &g->routes[ticket->route];
    if (r->inflight > 0) r->inflight--;
    r->last_status = resp->status;
    r->last_used_ms = now_ms;
  }
  if (ticket->bucket >= 0 && (uint32_t)ticket->bucket < GOV_BUCKETS) {
    gov_bucket *tb = &g->buckets[ticket->bucket];
    if (tb->used && tb->gen == ticket->bucket_gen && tb->inflight > 0) tb->inflight--;
  }

  // Learn the bucket from the response headers.
  gov_bucket *b = r ? route_bucket(g, r) : nullptr;
  if (r && resp->bucket[0] != '\0') {
    char bkey[GOV_KEY_MAX];
    size_t hl = strnlen(resp->bucket, sizeof resp->bucket);
    size_t ml = strlen(r->major);
    if (hl + 1u + ml < sizeof bkey) {
      memcpy(bkey, resp->bucket, hl);
      bkey[hl] = '|';
      memcpy(bkey + hl + 1u, r->major, ml + 1u);
      int32_t bi = gov_get_bucket(g, bkey, now_ms);
      route_attach(g, r, bi);
      b = &g->buckets[bi];
    }
  }
  if (r && !b && (resp->remaining >= 0 || resp->status == 429)) {  // provisional bucket keyed by the route
    int32_t bi = gov_get_bucket(g, r->key, now_ms);
    route_attach(g, r, bi);
    b = &g->buckets[bi];
  }
  if (b) {
    b->last_used_ms = now_ms;
    if (resp->limit > 0) b->limit = resp->limit;
    if (resp->remaining >= 0) {
      b->remaining = resp->remaining;
      if (resp->reset_after_ms > 0) b->reset_at_ms = now_ms + resp->reset_after_ms;
      else if (resp->remaining == 0) b->reset_at_ms = now_ms + 1000u;
    }
  }

  int st = resp->status;
  if (resp->network_error || st == 0) {
    g->stats.network_errors++;
    g->consecutive_failures++;
    g->backoff_until_ms = max_u64(g->backoff_until_ms, now_ms + gov_backoff_ms(g, g->consecutive_failures));
    out.should_retry = true;
    out.not_before_ms = g->backoff_until_ms;
    return out;
  }
  if (st == 429) {
    g->stats.rate_limited++;
    if (r) r->rate_limited++;
    g->consecutive_429++;
    uint64_t delay = max_u64(resp->retry_after_ms, resp->reset_after_ms);
    if (delay == 0) delay = 1000u;
    delay = gov_jitter(g, delay);
    if (resp->global) g->global_until_ms = max_u64(g->global_until_ms, now_ms + delay);
    else if (b) b->blocked_until_ms = max_u64(b->blocked_until_ms, now_ms + delay);
    if (g->consecutive_429 >= 3u) {
      g->backoff_until_ms = max_u64(g->backoff_until_ms, now_ms + gov_backoff_ms(g, g->consecutive_429 - 2u));
    }
    gov_note_invalid(g, now_ms);
    if (resp->edge_html) gov_trip_edge(g, now_ms);
    out.should_retry = true;
    out.not_before_ms = now_ms + delay;
    return out;
  }
  g->consecutive_429 = 0;
  if (st == 401) {
    g->stats.responses_4xx++;
    // A pre-auth 401 (bad password / bad TOTP code) is not a token rejection: no latch, no invalid budget.
    if (!resp->pre_auth) {
      g->auth_failed = true;
      gov_note_invalid(g, now_ms);
    }
    return out;
  }
  if (st == 403) {
    g->stats.responses_4xx++;
    gov_note_invalid(g, now_ms);
    if (resp->edge_html) gov_trip_edge(g, now_ms);
    return out;
  }
  if (st >= 500) {
    g->stats.responses_5xx++;
    g->consecutive_failures++;
    g->backoff_until_ms = max_u64(g->backoff_until_ms, now_ms + gov_backoff_ms(g, g->consecutive_failures));
    out.should_retry = true;
    out.not_before_ms = g->backoff_until_ms;
    return out;
  }
  // 2xx / 3xx / other 4xx: the service is healthy.
  if (st >= 400) g->stats.responses_4xx++;
  else g->stats.responses_2xx++;
  g->consecutive_failures = 0;
  if (now_ms >= g->edge_until_ms) g->edge_strikes = 0;
  return out;
}

void ccord_gov_cancel(ccord_gov *g, ccord_gov_ticket *ticket)
{
  if (!g || !ticket || !ticket->active) return;
  ticket->active = false;
  if (g->inflight > 0) g->inflight--;
  if (ticket->route >= 0 && (uint32_t)ticket->route < GOV_ROUTES) {
    gov_route *r = &g->routes[ticket->route];
    if (r->used && r->gen == ticket->route_gen && r->inflight > 0) r->inflight--;
  }
  if (ticket->bucket >= 0 && (uint32_t)ticket->bucket < GOV_BUCKETS) {
    gov_bucket *b = &g->buckets[ticket->bucket];
    if (b->used && b->gen == ticket->bucket_gen && b->inflight > 0) b->inflight--;
  }
}

void ccord_gov_reset_auth(ccord_gov *g)
{
  if (g) g->auth_failed = false;
}

bool ccord_gov_auth_failed(const ccord_gov *g) { return g && g->auth_failed; }

ccord_gov_stats ccord_gov_get_stats(const ccord_gov *g)
{
  ccord_gov_stats s = {0};
  if (!g) return s;
  s = g->stats;
  s.inflight = g->inflight;
  return s;
}

void ccord_gov_foreach_route(const ccord_gov *g, ccord_gov_route_cb cb, void *ud)
{
  if (!g || !cb) return;
  for (uint32_t i = 0; i < GOV_ROUTES; i++) {
    const gov_route *r = &g->routes[i];
    if (r->used) cb(ud, r->key, r->requests, r->rate_limited, r->last_status);
  }
}

const char *ccord_gov_reason_name(ccord_gov_reason r)
{
  switch (r) {
    case CCORD_GOV_REASON_NONE: return "none";
    case CCORD_GOV_REASON_INFLIGHT: return "inflight";
    case CCORD_GOV_REASON_BUCKET: return "bucket";
    case CCORD_GOV_REASON_GLOBAL: return "global";
    case CCORD_GOV_REASON_BACKOFF: return "backoff";
    case CCORD_GOV_REASON_TOKENS: return "tokens";
    case CCORD_GOV_REASON_EDGE: return "edge";
    case CCORD_GOV_REASON_AUTH: return "auth";
  }
  return "none";
}
