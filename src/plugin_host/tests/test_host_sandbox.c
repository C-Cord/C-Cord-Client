// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "sandbox.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/json.h>

#include <stdio.h>
#include <string.h>

#include "ccord_test.h"

typedef struct fake {
  int nreq;
  char method[64];
  char params[512];
  char log[256];
} fake;

static void f_request(void *ud, const char *method, const char *params)
{
  fake *f = ud;
  f->nreq++;
  snprintf(f->method, sizeof f->method, "%s", method);
  snprintf(f->params, sizeof f->params, "%s", params);
}

static void f_log(void *ud, ccord_log_level level, const char *text)
{
  fake *f = ud;
  (void)level;
  snprintf(f->log, sizeof f->log, "%s", text);
}

static ccord_sandbox *make(fake *f, ccord_perm_set perms, uint64_t mem, uint64_t ins)
{
  ccord_sandbox_config cfg = {
    .plugin_id = "dev.test.plugin", .plugin_name = "Test", .plugin_version = "1.0.0",
    .perms = perms, .memory_bytes = mem, .instructions = ins,
  };
  ccord_sandbox_services svc = {.ud = f, .request = f_request, .log = f_log};
  ccord_error err = {0};
  ccord_sandbox *sb = ccord_sandbox_create(&cfg, &svc, &err);
  CT_CHECK(sb != nullptr);
  return sb;
}

static ccord_status run(ccord_sandbox *sb, const char *src, ccord_error *err)
{
  return ccord_sandbox_load(sb, src, strlen(src), err);
}

static const ccord_json *parse(ccord_arena *a, const char *text)
{
  static const ccord_json_limits lim = {16, 8192, (size_t)1u << 20};
  ccord_error err = {0};
  const ccord_json *j = ccord_json_parse(a, text, strlen(text), &lim, &err);
  CT_CHECK(j != nullptr);
  return j;
}

int main(void)
{
  fake f = {0};
  ccord_error err = {0};

  // Library lockdown and read-only API tables.
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb, "assert(io == nil and os == nil and package == nil and debug == nil and dofile == nil and "
                     "load == nil and print == nil and string.dump == nil)\n"
                     "assert(require and events and ui and ui.toast and clipboard and storage and commands and log "
                     "and json and time and crypto and encoding and markdown and store and messages and fs and system "
                     "and plugin.id == 'dev.test.plugin')",
                 &err) == CCORD_OK);
    CT_CHECK(run(sb, "events.injected = 1", &err) != CCORD_OK);
    CT_CHECK(strstr(err.message, "read-only") != nullptr);
    ccord_sandbox_destroy(sb);
  }

  // Undeclared Class 2 / Class 3 calls are fatal (even if wrapped in pcall) and log [undeclared].
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    f.log[0] = '\0';
    CT_CHECK(run(sb, "pcall(function() messages.send('1', 'hi') end)", &err) == CCORD_OK);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
    CT_CHECK(strstr(f.log, "[undeclared]") != nullptr);
    ccord_sandbox_destroy(sb);
  }
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    f.log[0] = '\0';
    CT_CHECK(run(sb, "fs.read('/etc/hosts')", &err) == CCORD_ERR_INVALID);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
    CT_CHECK(strstr(f.log, "[undeclared]") != nullptr);
    ccord_sandbox_destroy(sb);
  }

  // Safe Class 1 APIs work without any manifest permissions.
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    f.nreq = 0;
    CT_CHECK(run(sb, "clipboard.write('hi')", &err) == CCORD_OK);
    CT_CHECK(f.nreq == 1);
    CT_CHECK(strcmp(f.method, "clipboard.write") == 0);
    CT_CHECK(strstr(f.params, "\"text\":\"hi\"") != nullptr);
    CT_CHECK(run(sb, "local enc = json.encode({a = 1, b = 'x'}); local dec = json.decode(enc); assert(dec.a == 1 and dec.b == 'x')", &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(#crypto.sha256('abc') == 64)", &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(encoding.base64_decode(encoding.base64_encode('hello')) == 'hello')", &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(markdown.escape('*hi*') == [[\\*hi\\*]])", &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);
  }

  // Explicit instruction budget still stops when configured (0 = unlimited).
  {
    ccord_sandbox *sb = make(&f, 0, 0, 10000);
    CT_CHECK(run(sb, "while true do end", &err) != CCORD_OK);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_INSTRUCTIONS);
    CT_CHECK(strstr(err.message, "instruction limit exceeded") != nullptr);
    ccord_sandbox_destroy(sb);
  }

  // Allocation past the 1 MiB cap stops the plugin on the memory limit.
  {
    ccord_sandbox *sb = make(&f, 0, 1u << 20, 0);
    CT_CHECK(run(sb, "local t = {} for i = 1, 100000000 do t[i] = string.rep('x', 1000) end", &err) != CCORD_OK);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_MEMORY);
    ccord_sandbox_destroy(sb);
  }

  // Event handler runs with the event data as a table and uses its permission.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_EVENTS_MESSAGES | CCORD_PERM_CLIPBOARD_WRITE, 0, 0);
    CT_CHECK(run(sb, "events.on('message.create', function(e) clipboard.write(e.content) end)", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "events.subscribe") == 0);
    ccord_arena a;
    ccord_arena_init(&a, 0);
    f.nreq = 0;
    CT_CHECK(ccord_sandbox_emit(sb, "message.create", parse(&a, "{\"content\":\"hello\"}"), &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "clipboard.write") == 0 && strstr(f.params, "\"text\":\"hello\"") != nullptr);
    ccord_arena_free(&a);
    ccord_sandbox_destroy(sb);
  }

  // Context menu: registration request, then the action runs with the context table.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_UI_CONTEXTMENU | CCORD_PERM_CLIPBOARD_WRITE, 0, 0);
    CT_CHECK(run(sb,
                 "ui.contextMenu.add({id = 'copy-message-id', label = 'Copy Message ID', target = 'message',"
                 " action = function(ctx) clipboard.write(ctx.message_id) end})",
                 &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.contextMenu.add") == 0);
    ccord_arena a;
    ccord_arena_init(&a, 0);
    f.nreq = 0;
    CT_CHECK(ccord_sandbox_menu_invoke(sb, "copy-message-id", parse(&a, "{\"message_id\":\"42\"}"), &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "clipboard.write") == 0 && strstr(f.params, "\"text\":\"42\"") != nullptr);
    ccord_arena_free(&a);
    ccord_sandbox_destroy(sb);
  }

  // Storage: values are cached locally, keys are validated, and set is forwarded to main.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_STORAGE, 0, 0);
    CT_CHECK(run(sb, "storage.set('seen', 'yes'); assert(storage.get('seen') == 'yes'); assert(#storage.keys() == 1)", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "storage.set") == 0);
    CT_CHECK(run(sb, "storage.set('bad key', 1)", &err) != CCORD_OK);
    CT_CHECK(strstr(err.message, "storage.set: key must match") != nullptr);
    ccord_sandbox_destroy(sb);
  }

  // Commands: the registered function's string result is returned to the host.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_COMMANDS, 0, 0);
    CT_CHECK(run(sb, "commands.register('ping', 'Reply pong', function(args, ctx) return 'pong ' .. args end)", &err) == CCORD_OK);
    ccord_arena a;
    ccord_arena_init(&a, 0);
    char *text = nullptr;
    CT_CHECK(ccord_sandbox_command_run(sb, "ping", "x", parse(&a, "{}"), &text, &err) == CCORD_OK);
    CT_CHECK(text && strcmp(text, "pong x") == 0);
    ccord_free(text);
    ccord_arena_free(&a);
    ccord_sandbox_destroy(sb);
  }

  // Undeclared ui.panel.mount is fatal and logs [undeclared].
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    f.log[0] = '\0';
    CT_CHECK(run(sb, "ui.panel.mount({id = 'bar', root = {type = 'row'}})", &err) == CCORD_ERR_INVALID);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
    CT_CHECK(strstr(f.log, "[undeclared]") != nullptr);
    ccord_sandbox_destroy(sb);
  }

  // ui.panel mount, update, event dispatch, and unmount.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_UI_PANEL | CCORD_PERM_CLIPBOARD_WRITE, 0, 0);
    f.nreq = 0;
    CT_CHECK(run(sb,
                 "ui.panel.mount({id = 'top-bar', slot = 'top', height = 36,"
                 " root = {type = 'row', children = {{type = 'button', id = 'btn-1', label = 'Go'}}},"
                 " on_event = function(ev) clipboard.write(ev.type .. ':' .. (ev.id or '')) end})",
                 &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.panel.mount") == 0);
    CT_CHECK(strstr(f.params, "\"id\":\"top-bar\"") != nullptr && strstr(f.params, "\"slot\":\"top\"") != nullptr);

    CT_CHECK(run(sb, "ui.panel.update('top-bar', {height = 40, root = {type = 'row', children = {}}})", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.panel.update") == 0);
    CT_CHECK(strstr(f.params, "\"height\":40") != nullptr);

    ccord_arena a;
    ccord_arena_init(&a, 0);
    f.nreq = 0;
    CT_CHECK(ccord_sandbox_panel_event(sb, "top-bar", parse(&a, "{\"type\":\"click\",\"id\":\"btn-1\",\"button\":1}"), &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "clipboard.write") == 0 && strstr(f.params, "\"text\":\"click:btn-1\"") != nullptr);

    CT_CHECK(run(sb, "ui.panel.unmount('top-bar')", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.panel.unmount") == 0);
    f.nreq = 0;
    CT_CHECK(ccord_sandbox_panel_event(sb, "top-bar", parse(&a, "{\"type\":\"click\",\"id\":\"btn-1\"}"), &err) == CCORD_OK);
    CT_CHECK(f.nreq == 0);
    ccord_arena_free(&a);
    ccord_sandbox_destroy(sb);
  }

  // Context menu 'nav' target and ui.contextMenu.remove.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_UI_CONTEXTMENU | CCORD_PERM_CLIPBOARD_WRITE, 0, 0);
    CT_CHECK(run(sb,
                 "ui.contextMenu.add({id = 'open-aux', label = 'Open Aux', target = 'nav',"
                 " action = function(ctx) clipboard.write(ctx.view or '') end})",
                 &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.contextMenu.add") == 0 && strstr(f.params, "\"target\":\"nav\"") != nullptr);
    ccord_arena a;
    ccord_arena_init(&a, 0);
    f.nreq = 0;
    CT_CHECK(ccord_sandbox_menu_invoke(sb, "open-aux", parse(&a, "{\"kind\":\"nav\",\"view\":\"friends\"}"), &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "clipboard.write") == 0 && strstr(f.params, "\"text\":\"friends\"") != nullptr);
    CT_CHECK(run(sb, "ui.contextMenu.remove('open-aux')", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.contextMenu.remove") == 0);
    ccord_arena_free(&a);
    ccord_sandbox_destroy(sb);
  }

  // Navigation extensions and navigation/channel events.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_NAVIGATION_WRITE | CCORD_PERM_EVENTS_NAVIGATION | CCORD_PERM_EVENTS_CHANNELS, 0, 0);
    CT_CHECK(run(sb, "navigation.select_channel('900', {guild_id = '10', parent_id = '20', name = 'Thread'})", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "navigation.select_channel") == 0 && strstr(f.params, "\"parent_id\":\"20\"") != nullptr);
    CT_CHECK(run(sb, "navigation.select_view('friends')", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "navigation.select_view") == 0 && strstr(f.params, "\"view\":\"friends\"") != nullptr);
    CT_CHECK(run(sb, "navigation.action('search')", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "navigation.action") == 0 && strstr(f.params, "\"action\":\"search\"") != nullptr);
    CT_CHECK(run(sb, "navigation.open_quick_switcher()", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "navigation.action") == 0 && strstr(f.params, "\"action\":\"quick_switcher\"") != nullptr);
    CT_CHECK(run(sb, "navigation.select_view('bogus')", &err) != CCORD_OK);
    CT_CHECK(run(sb, "events.on('view.select', function() end); events.on('navigation.aux_open', function() end); events.on('notify.update', function() end)", &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);
  }

  // Security regressions (P1, P2, P6, P12, P16):
  // 2. Deeply nested JSON/Lua stack round-trip under LUA_USE_APICHECK (P1).
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb,
                 "local s = string.rep('{\"a\":', 30) .. '1' .. string.rep('}', 30)\n"
                 "local t = json.decode(s)\n"
                 "local enc = json.encode(t)\n"
                 "assert(type(enc) == 'string' and #enc > 60)",
                 &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);
  }


  return CT_RESULT("test_host_sandbox");
}
