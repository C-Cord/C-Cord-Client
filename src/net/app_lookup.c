// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Application lookup sources for local RPC rich presence (see ccord/net/app_lookup.h).
#include <ccord/net/app_lookup.h>

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>
#include <ccord/net/live_transport.h>

#include <stdio.h>
#include <string.h>

static void copy_utf8(char *dst, size_t cap, const char *src, size_t len)
{
  if (len >= cap) {
    len = cap - 1u;
    while (len > 0 && ((unsigned char)src[len] & 0xC0u) == 0x80u) len--;   // never cut inside a UTF-8 sequence
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static bool valid_snowflake(const char *s)
{
  size_t n = s ? strlen(s) : 0;
  if (n == 0 || n >= CCORD_APP_LOOKUP_ID_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

ccord_status ccord_app_lookup_parse(const char *body, size_t len, const char *client_id, ccord_app_info *out,
                                    ccord_error *err)
{
  ccord_error_clear(err);
  if (!body || !client_id || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "app_lookup: missing argument");
  memset(out, 0, sizeof *out);
  ccord_arena arena;
  size_t hint = len > 16384u ? 16384u : (len + 512u < 2048u ? 2048u : len + 512u);
  ccord_arena_init(&arena, hint);
  ccord_error perr;
  const ccord_json *j = ccord_json_parse(&arena, body, len, nullptr, &perr);
  ccord_status st = CCORD_OK;
  if (!j || ccord_json_type_of(j) != CCORD_JSON_OBJECT) {
    st = ccord_error_set(err, CCORD_ERR_PROTOCOL, "app_lookup: response is not a JSON object");
  } else {
    size_t idn = 0, nn = 0, icn = 0;
    const ccord_json *jid = ccord_json_get(j, "id"), *jname = ccord_json_get(j, "name"), *jicon = ccord_json_get(j, "icon");
    const char *id = jid ? ccord_json_string(jid, &idn) : nullptr;
    const char *name = jname ? ccord_json_string(jname, &nn) : nullptr;
    const char *icon = jicon ? ccord_json_string(jicon, &icn) : nullptr;   // null/absent icon is fine
    if (!id || idn != strlen(client_id) || memcmp(id, client_id, idn) != 0) {
      st = ccord_error_set(err, CCORD_ERR_PROTOCOL, "app_lookup: response is for another application");
    } else if (!name || nn == 0) {
      st = ccord_error_set(err, CCORD_ERR_PROTOCOL, "app_lookup: application has no name");
    } else {
      copy_utf8(out->id, sizeof out->id, id, idn);
      copy_utf8(out->name, sizeof out->name, name, nn);
      if (icon) copy_utf8(out->icon, sizeof out->icon, icon, icn);
    }
  }
  ccord_arena_free(&arena);
  return st;
}

// ------------------------------------------------------------------ fixture (--mock)

#define MOCK_PENDING_MAX 16u

typedef struct mock_pending {
  char client_id[CCORD_APP_LOOKUP_ID_MAX];
  ccord_app_lookup_done done;
  void *ud;
} mock_pending;

typedef struct mock_ctx {
  mock_pending pending[MOCK_PENDING_MAX];
  size_t count;
} mock_ctx;

static ccord_status mock_get(void *ctx, const char *client_id, ccord_app_lookup_done done, void *ud, ccord_error *err)
{
  mock_ctx *m = ctx;
  ccord_error_clear(err);
  if (!valid_snowflake(client_id) || !done) return ccord_error_set(err, CCORD_ERR_INVALID, "app_lookup: bad client id");
  if (m->count >= MOCK_PENDING_MAX) return ccord_error_set(err, CCORD_ERR_RANGE, "app_lookup: too many lookups");
  mock_pending *p = &m->pending[m->count++];
  snprintf(p->client_id, sizeof p->client_id, "%s", client_id);
  p->done = done;
  p->ud = ud;
  return CCORD_OK;
}

static void mock_deliver(mock_ctx *m, bool fail)
{
  if (!m || m->count == 0) return;
  mock_pending batch[MOCK_PENDING_MAX];
  size_t n = m->count;
  memcpy(batch, m->pending, n * sizeof batch[0]);
  m->count = 0;   // callbacks may start new lookups
  for (size_t i = 0; i < n; i++) {
    if (fail) {
      batch[i].done(batch[i].ud, CCORD_ERR_IO, nullptr);
      continue;
    }
    ccord_app_info info = {0};
    memcpy(info.id, batch[i].client_id, sizeof info.id);   // both CCORD_APP_LOOKUP_ID_MAX, NUL-terminated by mock_get
    snprintf(info.name, sizeof info.name, "%s", CCORD_APP_LOOKUP_FIXTURE_NAME);
    snprintf(info.icon, sizeof info.icon, "fixtureicon");
    batch[i].done(batch[i].ud, CCORD_OK, &info);
  }
}

static void mock_pump(void *ctx, uint64_t now_ms)
{
  (void)now_ms;
  mock_ctx *m = ctx;
  if (!m || m->count == 0) return;
  mock_deliver(m, false);
}

static void mock_destroy(void *ctx)
{
  mock_deliver(ctx, true);
  ccord_free(ctx);
}

ccord_app_lookup_source ccord_app_lookup_mock_source(void)
{
  mock_ctx *m = ccord_calloc(1, sizeof *m);
  if (!m) return (ccord_app_lookup_source){0};
  return (ccord_app_lookup_source){.get = mock_get, .pump = mock_pump, .destroy = mock_destroy, .ctx = m};
}

// ------------------------------------------------------------------ live REST

#if defined(CCORD_APP_LOOKUP_REST)
typedef struct rest_ctx {
  ccord_rest_queue *queue;
} rest_ctx;

typedef struct rest_job {
  char client_id[CCORD_APP_LOOKUP_ID_MAX];
  ccord_app_lookup_done done;
  void *ud;
} rest_job;

static void rest_cb(void *ud, ccord_status status, long http_status, const char *body, size_t body_len)
{
  rest_job *job = ud;
  ccord_app_info info = {0};
  ccord_status st = status;
  if (st == CCORD_OK && (http_status < 200 || http_status >= 300)) st = CCORD_ERR_PROTOCOL;   // 404: unknown application
  if (st == CCORD_OK) st = ccord_app_lookup_parse(body ? body : "", body ? body_len : 0, job->client_id, &info, nullptr);
  job->done(job->ud, st, st == CCORD_OK ? &info : nullptr);
  ccord_free(job);
}

static ccord_status rest_get(void *ctx, const char *client_id, ccord_app_lookup_done done, void *ud, ccord_error *err)
{
  rest_ctx *r = ctx;
  ccord_error_clear(err);
  if (!valid_snowflake(client_id) || !done) return ccord_error_set(err, CCORD_ERR_INVALID, "app_lookup: bad client id");
  if (!r->queue) {
    ccord_rest_config cfg = {.timeout_ms = 10000u, .max_response_bytes = 64u * 1024u};   // no token: the endpoint is public
    r->queue = ccord_rest_queue_create(&cfg, err);
    if (!r->queue) return err && err->code != CCORD_OK ? err->code : CCORD_ERR_IO;
  }
  rest_job *job = ccord_calloc(1, sizeof *job);
  if (!job) return ccord_error_set(err, CCORD_ERR_NOMEM, "app_lookup: out of memory");
  snprintf(job->client_id, sizeof job->client_id, "%s", client_id);
  job->done = done;
  job->ud = ud;
  char path[64];
  (void)snprintf(path, sizeof path, "/oauth2/applications/%s/rpc", client_id);   // digits only: no escaping needed
  ccord_status st = ccord_rest_queue_add(r->queue, CCORD_REST_GET, path, nullptr, rest_cb, job, err);
  if (st != CCORD_OK) {
    ccord_free(job);   // a failed add never calls the callback
    if (ccord_rest_queue_pending(r->queue) == 0) {
      ccord_rest_queue_destroy(r->queue);
      r->queue = nullptr;
    }
  }
  return st;
}

static void rest_pump(void *ctx, uint64_t now_ms)
{
  rest_ctx *r = ctx;
  if (!r || !r->queue) return;
  if (ccord_rest_queue_pending(r->queue) == 0) {
    ccord_rest_queue_destroy(r->queue);
    r->queue = nullptr;
    return;
  }
  ccord_rest_queue_pump(r->queue, now_ms);
  if (ccord_rest_queue_pending(r->queue) == 0) {
    ccord_rest_queue_destroy(r->queue);
    r->queue = nullptr;
  }
}

static void rest_destroy(void *ctx)
{
  rest_ctx *r = ctx;
  if (!r) return;
  ccord_rest_queue_destroy(r->queue);   // fails every pending job through rest_cb
  ccord_free(r);
}
#endif

ccord_app_lookup_source ccord_app_lookup_rest_source(void)
{
#if defined(CCORD_APP_LOOKUP_REST)
  rest_ctx *r = ccord_calloc(1, sizeof *r);
  if (!r) return (ccord_app_lookup_source){0};
  return (ccord_app_lookup_source){.get = rest_get, .pump = rest_pump, .destroy = rest_destroy, .ctx = r};
#else
  return (ccord_app_lookup_source){0};
#endif
}
