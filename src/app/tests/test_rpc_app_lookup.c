// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Application-name lookup rules for local RPC rich presence: resolve, pending/failed publish nothing, the client's
// name after a failed lookup, one lookup per client_id, retry with backoff, and the response parser.
#include "rpc_app_lookup.h"

#include <ccord/net/app_lookup.h>

#include <string.h>

#include "ccord_test.h"

typedef struct script {
  ccord_app_lookup_source inner;
  unsigned gets;
  bool fail;
} script;

static ccord_status s_get(void *ctx, const char *id, ccord_app_lookup_done done, void *ud, ccord_error *err)
{
  script *s = ctx;
  s->gets++;
  if (s->fail) return ccord_error_set(err, CCORD_ERR_IO, "scripted");
  return s->inner.get(s->inner.ctx, id, done, ud, err);
}
static void s_pump(void *ctx, uint64_t now) { ((script *)ctx)->inner.pump(((script *)ctx)->inner.ctx, now); }
static void s_destroy(void *ctx) { ((script *)ctx)->inner.destroy(((script *)ctx)->inner.ctx); }

static ccord_rpc_activity act(const char *name)
{
  ccord_rpc_activity a = {0};
  snprintf(a.name, sizeof a.name, "%s", name);
  snprintf(a.details, sizeof a.details, "details");
  return a;
}

static void test_resolve_and_cache(void)
{
  script s = {.inner = ccord_app_lookup_mock_source()};
  ccord_rpc_app_lookup *l = ccord_rpc_app_lookup_create(
    (ccord_app_lookup_source){.get = s_get, .pump = s_pump, .destroy = s_destroy, .ctx = &s});
  CT_CHECK(l != nullptr);
  ccord_rpc_activity a = act("");
  CT_CHECK(!ccord_rpc_app_lookup_resolve(l, "42", &a, 0));   // pending: nothing
  CT_CHECK(ccord_rpc_app_lookup_poll(l, 1));                 // fixture answered
  a = act("");
  CT_CHECK(ccord_rpc_app_lookup_resolve(l, "42", &a, 2));
  CT_CHECK(strcmp(a.name, CCORD_APP_LOOKUP_FIXTURE_NAME) == 0 && strcmp(a.application_id, "42") == 0);
  CT_CHECK(strcmp(a.details, "details") == 0);
  a = act("Client name");   // the application's name wins over what the client sent
  CT_CHECK(ccord_rpc_app_lookup_resolve(l, "42", &a, 3) && strcmp(a.name, CCORD_APP_LOOKUP_FIXTURE_NAME) == 0);
  CT_CHECK(s.gets == 1 && ccord_rpc_app_lookup_started(l) == 1);
  ccord_rpc_app_lookup_destroy(l);
}

static void test_failure_fallback_and_backoff(void)
{
  script s = {.inner = ccord_app_lookup_mock_source(), .fail = true};
  ccord_rpc_app_lookup *l = ccord_rpc_app_lookup_create(
    (ccord_app_lookup_source){.get = s_get, .pump = s_pump, .destroy = s_destroy, .ctx = &s});
  CT_CHECK(l != nullptr);
  ccord_rpc_activity a = act("");
  CT_CHECK(!ccord_rpc_app_lookup_resolve(l, "7", &a, 1000));   // failed, no client name: nothing
  a = act("Mine");
  CT_CHECK(ccord_rpc_app_lookup_resolve(l, "7", &a, 1001));    // failed, client name: used
  CT_CHECK(strcmp(a.name, "Mine") == 0 && strcmp(a.application_id, "7") == 0);
  CT_CHECK(s.gets == 1);
  CT_CHECK(ccord_rpc_app_lookup_poll(l, 1500));                   // reports the failure once
  CT_CHECK(!ccord_rpc_app_lookup_poll(l, 5999) && s.gets == 1);   // inside the 5 s backoff
  (void)ccord_rpc_app_lookup_poll(l, 6001);                       // retry #1 (fails again)
  CT_CHECK(s.gets == 2);
  (void)ccord_rpc_app_lookup_poll(l, 6001 + 9999);                // backoff doubled to 10 s
  CT_CHECK(s.gets == 2);
  s.fail = false;
  (void)ccord_rpc_app_lookup_poll(l, 6001 + 10001);               // retry #2 succeeds on the next pump
  CT_CHECK(s.gets == 3);
  CT_CHECK(ccord_rpc_app_lookup_poll(l, 6001 + 10002));
  a = act("");
  CT_CHECK(ccord_rpc_app_lookup_resolve(l, "7", &a, 20000) && strcmp(a.name, CCORD_APP_LOOKUP_FIXTURE_NAME) == 0);
  ccord_rpc_app_lookup_destroy(l);
}

static void test_parse(void)
{
  ccord_app_info info;
  ccord_error err;
  static const char ok[] = "{\"id\":\"42\",\"name\":\"Real Game\",\"icon\":\"abc\",\"description\":\"x\"}";
  CT_CHECK(ccord_app_lookup_parse(ok, strlen(ok), "42", &info, &err) == CCORD_OK);
  CT_CHECK(strcmp(info.name, "Real Game") == 0 && strcmp(info.icon, "abc") == 0 && strcmp(info.id, "42") == 0);
  CT_CHECK(ccord_app_lookup_parse(ok, strlen(ok), "43", &info, &err) == CCORD_ERR_PROTOCOL);   // another application
  static const char noname[] = "{\"id\":\"42\",\"icon\":null}";
  CT_CHECK(ccord_app_lookup_parse(noname, strlen(noname), "42", &info, &err) == CCORD_ERR_PROTOCOL);
  static const char noicon[] = "{\"id\":\"42\",\"name\":\"G\",\"icon\":null}";
  CT_CHECK(ccord_app_lookup_parse(noicon, strlen(noicon), "42", &info, &err) == CCORD_OK && info.icon[0] == '\0');
  CT_CHECK(ccord_app_lookup_parse("{\"message\":\"Unknown Application\",\"code\":10002}", 44, "42", &info, &err) ==
           CCORD_ERR_PROTOCOL);
  CT_CHECK(ccord_app_lookup_parse("nope", 4, "42", &info, &err) == CCORD_ERR_PROTOCOL);
}

int main(void)
{
  test_resolve_and_cache();
  test_failure_fallback_and_backoff();
  test_parse();
  return CT_RESULT("test_rpc_app_lookup");
}
