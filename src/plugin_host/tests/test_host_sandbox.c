// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "sandbox/sandbox.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static bool f_request_sync(void *ud, const char *method, const char *params,
                           ccord_arena *a, const ccord_json **out, char *out_err, size_t err_cap)
{
  fake *f = ud;
  f->nreq++;
  snprintf(f->method, sizeof f->method, "%s", method);
  snprintf(f->params, sizeof f->params, "%s", params);
  if (out) {
    if (strcmp(method, "ui.media.open") == 0 || strcmp(method, "ui.open_user_profile") == 0 ||
        strcmp(method, "ui.menu.popup") == 0) {
      *out = ccord_json_parse(a, "true", 4, nullptr, nullptr);
    } else {
      *out = ccord_json_parse(a, "[]", 2, nullptr, nullptr);
    }
  }
  if (out_err && err_cap > 0) out_err[0] = '\0';
  return true;
}

static ccord_sandbox *make(fake *f, ccord_perm_set perms, uint64_t mem, uint64_t ins)
{
  ccord_sandbox_config cfg = {
    .plugin_id = "dev.test.plugin", .plugin_name = "Test", .plugin_version = "1.0.0",
    .perms = perms, .memory_bytes = mem, .instructions = ins,
  };
  ccord_sandbox_services svc = {.ud = f, .request = f_request, .request_sync = f_request_sync, .log = f_log};
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
  // 1. Frozen metatables on read-only API tables (P6).
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb, "assert(getmetatable(messages) == false and getmetatable(events) == false and getmetatable(ui) == false)", &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);
  }

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

  // 3. __index metamethod returning fresh strings cannot cause popped-string UAF (P1).
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_UI_CONTEXTMENU | CCORD_PERM_UI_PANEL | CCORD_PERM_SYSTEM_NOTIFY, 0, 0);
    CT_CHECK(run(sb,
                 "local t = setmetatable({}, { __index = function(_, k) collectgarbage('collect'); return tostring(k) .. '_dyn' end })\n"
                 "local ok1 = pcall(function() ui.contextMenu.add(t) end)\n"
                 "local ok2 = pcall(function() ui.panel.mount(t) end)\n"
                 "local ok3 = pcall(function() system.notify(t) end)\n"
                 "assert(not ok1 and not ok2 and not ok3)",
                 &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);
  }

  // 4. Child coroutines inherit instruction limits & sticky undeclared-API termination (P2).
  {
    ccord_sandbox *sb = make(&f, 0, 0, 20000);
    CT_CHECK(run(sb, "local co = coroutine.create(function() while true do end end); local ok, e = coroutine.resume(co); assert(not ok)", &err) != CCORD_OK ||
             ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_INSTRUCTIONS);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_INSTRUCTIONS);
    ccord_sandbox_destroy(sb);

    sb = make(&f, CCORD_PERM_CLIPBOARD_WRITE, 0, 0);
    f.nreq = 0;
    CT_CHECK(run(sb, "pcall(function() messages.send('1', 'nope') end); clipboard.write('after'); for i = 1, 20000 do end", &err) != CCORD_OK);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
    CT_CHECK(f.nreq == 0);
    ccord_sandbox_destroy(sb);
  }

  // 5. Timer count cap (SB_MAX_TIMERS = 256) rejects excess timers cleanly (P12).
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb,
                 "for i = 1, 256 do time.after(1000, function() end) end\n"
                 "local ok = pcall(function() time.after(1000, function() end) end)\n"
                 "assert(not ok)",
                 &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);
  }

  // 6. require() rejects symlinks escaping plugin_dir (P16a).
  {
    char pdir[] = "/tmp/ccord-sb-req-XXXXXX";
    CT_CHECK(mkdtemp(pdir) != nullptr);
    char outside[256], linkpath[256];
    snprintf(outside, sizeof outside, "%s-outside.lua", pdir);
    snprintf(linkpath, sizeof linkpath, "%s/escape.lua", pdir);
    FILE *of = fopen(outside, "w");
    CT_CHECK(of != nullptr);
    if (of) {
      fputs("return 42\n", of);
      fclose(of);
    }
    CT_CHECK(symlink(outside, linkpath) == 0);
    ccord_sandbox_config cfg = {
      .plugin_id = "dev.test.req", .plugin_name = "Req", .plugin_version = "1.0.0",
      .plugin_dir = pdir,
    };
    ccord_sandbox_services svc = {.ud = &f, .request = f_request, .log = f_log};
    ccord_sandbox *sb = ccord_sandbox_create(&cfg, &svc, &err);
    CT_CHECK(sb != nullptr);
    CT_CHECK(run(sb, "require('escape')", &err) != CCORD_OK);
    ccord_sandbox_destroy(sb);
    unlink(linkpath);
    unlink(outside);
    rmdir(pdir);
  }

  // ---- API v3 Sandbox Tests ----

  // 1. Pure Class 1 libs: color.*, compress.*, semver.*, emoji.*, markdown.parse, async.*
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    ccord_status st1 = run(sb,
                 "local c = color.parse('#5865f2')\n"
                 "assert(c.r == 0x58 and c.g == 0x65 and c.b == 0xf2 and c.a == 255)\n"
                 "assert(color.to_hex(c) == '#5865f2')\n"
                 "local h = color.to_hsl('#ff0000')\n"
                 "assert(math.abs(h.h) < 1 and math.abs(h.s - 1) < 0.01 and math.abs(h.l - 0.5) < 0.01)\n"
                 "assert(color.from_hsl(0, 1, 0.5) == '#ff0000')\n"
                 "assert(color.lighten('#000000', 0.5) == '#808080')\n"
                 "assert(color.darken('#ffffff', 0.5) == '#808080')\n"
                 "assert(color.mix('#000000', '#ffffff', 0.5) == '#808080')\n"
                 "assert(color.alpha('#112233', 0.5) == '#11223380')\n"
                 "assert(color.contrast('#ffffff', '#000000') > 20.9)\n"
                 "local src = string.rep('hello c-cord plugin v3 ', 40)\n"
                 "local z = compress.deflate(src)\n"
                 "assert(#z < #src and compress.inflate(z) == src)\n"
                 "local gz = compress.gzip(src)\n"
                 "assert(#gz < #src and compress.gunzip(gz) == src)\n"
                 "local sv = semver.parse('v2.3.4-beta.1')\n"
                 "assert(sv.major == 2 and sv.minor == 3 and sv.patch == 4 and sv.prerelease == 'beta.1')\n"
                 "assert(semver.compare('1.2.3', '1.2.4') == -1 and semver.compare('2.0.0', '1.9.9') == 1)\n"
                 "assert(semver.satisfies('1.4.0', '^1.2.0') and not semver.satisfies('2.0.0', '^1.2.0'))\n"
                 "assert(semver.satisfies('1.2.9', '~1.2.3') and not semver.satisfies('1.3.0', '~1.2.3'))\n"
                 "local em = emoji.lookup(':fire:')\n"
                 "assert(em and em.name == 'fire' and #em.unicode > 0)\n"
                 "local sc = emoji.to_shortcode(em.unicode)\n"
                 "assert(sc == 'fire')\n"
                 "local hits = emoji.search('smile', 5)\n"
                 "assert(#hits >= 1)\n"
                 "assert(emoji.replace_shortcodes('lit :fire:!') == 'lit ' .. em.unicode .. '!')\n"
                 "local toks = markdown.parse('hi **bold** `code` <@12345> https://example.com ||sec||')\n"
                 "local seen = {}\n"
                 "for _, t in ipairs(toks) do seen[t.type] = t.text end\n"
                 "assert(seen.bold == 'bold' and seen.code == 'code' and seen.user_mention == '<@12345>' and seen.link == 'https://example.com' and seen.spoiler == 'sec')\n",
                 &err);
    if (st1 != CCORD_OK) fprintf(stderr, "v3 libs err: %s\n", err.message);
    CT_CHECK(st1 == CCORD_OK);
    // async.spawn + async.sleep coroutine scheduling
    CT_CHECK(run(sb,
                 "ran_step = 0\n"
                 "async.spawn(function()\n"
                 "  ran_step = 1\n"
                 "  async.sleep(10)\n"
                 "  ran_step = 2\n"
                 "end)\n"
                 "assert(ran_step == 1)\n",
                 &err) == CCORD_OK);
    uint64_t tnow = ccord_clock_ms();
    CT_CHECK(ccord_sandbox_tick(sb, tnow, &err) == CCORD_OK);
    CT_CHECK(ccord_sandbox_tick(sb, tnow + 50, &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(ran_step == 2)", &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);
  }

  // 2. plugin.read_asset / asset_exists / list_assets + path traversal rejection
  {
    char pdir[] = "/tmp/ccord-sb-asset-XXXXXX";
    CT_CHECK(mkdtemp(pdir) != nullptr);
    char asset_p[256], outside_p[256];
    snprintf(asset_p, sizeof asset_p, "%s/hello.txt", pdir);
    snprintf(outside_p, sizeof outside_p, "%s-secret.txt", pdir);
    FILE *af = fopen(asset_p, "wb");
    CT_CHECK(af != nullptr);
    if (af) { fputs("asset-data-v3", af); fclose(af); }
    FILE *of = fopen(outside_p, "wb");
    CT_CHECK(of != nullptr);
    if (of) { fputs("top-secret", of); fclose(of); }

    ccord_sandbox_config cfg = {
      .plugin_id = "dev.test.asset", .plugin_name = "Asset", .plugin_version = "1.0.0",
      .plugin_dir = pdir,
    };
    ccord_sandbox_services svc = {.ud = &f, .request = f_request, .log = f_log};
    ccord_sandbox *sb = ccord_sandbox_create(&cfg, &svc, &err);
    CT_CHECK(sb != nullptr);
    CT_CHECK(run(sb,
                 "assert(plugin.asset_exists('hello.txt') == true)\n"
                 "assert(plugin.read_asset('hello.txt') == 'asset-data-v3')\n"
                 "assert(plugin.asset_exists('../escape') == false)\n"
                 "local bad, berr = plugin.read_asset('../escape')\n"
                 "assert(bad == nil and type(berr) == 'string')\n"
                 "local bad2 = plugin.read_asset('/etc/passwd')\n"
                 "assert(bad2 == nil)\n"
                 "local list = plugin.list_assets('')\n"
                 "assert(type(list) == 'table' and #list >= 1)\n",
                 &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);
    unlink(asset_p);
    unlink(outside_p);
    rmdir(pdir);
  }

  // 3. Class 1 settings.schema / get / set / on_change, theme.register, ui.dialog user-gesture guard
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_COMMANDS, 0, 0);
    f.nreq = 0;
    ccord_status st3 = run(sb,
                 "settings.schema({{key='enabled', type='toggle', label='Enable', default=true},"
                 " {key='limit', type='slider', label='Limit', min=1, max=10, default=5}})\n"
                 "assert(settings.get('enabled') == true and settings.get('limit') == 5)\n"
                 "settings.set('limit', 8)\n"
                 "assert(settings.get('limit') == 8 and settings.all().limit == 8)\n"
                 "chg_k, chg_v = nil, nil\n"
                 "settings.on_change(function(k, v) chg_k = k; chg_v = v end)\n"
                 "theme.register({id='neon', name='Neon', base='c-cord-dark', css=':root{--brand-experiment:#ff00ff;}'})\n"
                 "-- ui.dialog outside user gesture MUST be rejected for Class 1\n"
                 "local d, derr = ui.dialog.confirm({title='Spoof', message='Click ok'})\n"
                 "assert(d == nil and derr and derr:find('user%-gesture'))\n"
                 "-- ui.dialog inside a user-gesture command callback MUST succeed\n"
                 "dlg_req = nil\n"
                 "commands.register('ask', 'Ask user', function()\n"
                 "  dlg_req = ui.dialog.confirm({title='Confirm', message='Proceed?'}, function(res) dlg_res = res end)\n"
                 "end)\n",
                 &err);
    if (st3 != CCORD_OK) fprintf(stderr, "v3 settings/dlg err: %s\n", err.message);
    CT_CHECK(st3 == CCORD_OK);
    // Notify setting change from host
    ccord_arena ar; ccord_arena_init(&ar, 256u);
    const ccord_json_limits lim = {8, 64, 1024u};
    const ccord_json *jv = ccord_json_parse(&ar, "false", 5, &lim, &err);
    CT_CHECK(ccord_sandbox_setting_changed(sb, "enabled", jv, &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(chg_k == 'enabled' and chg_v == false and settings.get('enabled') == false)", &err) == CCORD_OK);
    // Run command (sets user_gesture_depth) -> opens dialog -> resolve dialog
    const ccord_json *ctx = ccord_json_parse(&ar, "{\"channel_id\":\"10\"}", 19, &lim, &err);
    char *out_json = nullptr;
    CT_CHECK(ccord_sandbox_command_run_ex(sb, "ask", "", ctx, &out_json, &err) == CCORD_OK);
    ccord_free(out_json);
    CT_CHECK(run(sb, "assert(type(dlg_req) == 'number' and dlg_req > 0)", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.dialog.open") == 0);
    ccord_arena_free(&ar);
    ccord_sandbox_destroy(sb);
  }

  // 4. Undeclared Class 2/3 v3 permissions are strictly fatal (CCORD_SB_LIMIT_UNDECLARED).
  {
    static const char *const k_v3_forbidden[] = {
      "theme.set_base('c-cord-amoled')",
      "theme.patch_colors({bg_primary='#000000'})",
      "theme.inject_css('l1', ':root{}')",
      "ui.decorate.message_badge({id='b', widget={type='text', text='x'}})",
      "ui.composer.get_text()",
      "audio.override_earcon('mention', 'RIFF')",
      "hooks.on_notification(function() end)",
      "hooks.on_link_open(function() end)",
      "social.set_user_note('123', 'hi')",
      "window.set_title('evil')",
      "voice.levels()",
      "net.ws.connect('wss://example.com')",
      "net.local_net.request({url='http://127.0.0.1:8080'})",
      "fs.watch('/tmp', function() end)",
      "system.exec({cmd={'/bin/true'}})",
      "system.env('HOME')",
      "system.shortcuts.register('g1', 'CTRL+SHIFT+1', function() end)",
      "system.media.current()",
      "system.processes.list()",
      "clipboard.read_image()",
    };
    for (size_t i = 0; i < sizeof k_v3_forbidden / sizeof k_v3_forbidden[0]; i++) {
      ccord_sandbox *sb = make(&f, 0, 0, 0);
      f.nreq = 0;
      CT_CHECK(run(sb, k_v3_forbidden[i], &err) != CCORD_OK);
      CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
      CT_CHECK(f.nreq == 0);
      ccord_sandbox_destroy(sb);
    }
  }

  // 5. Background mode suspends periodic time.every() timers and enforces a >= 50 ms interval floor.
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb, "hits = 0; time.every(1, function() hits = hits + 1 end)", &err) == CCORD_OK);
    uint64_t t0 = ccord_clock_ms();
    // 1 ms request is floored to >= 50 ms, so ticking at +10 ms does not fire, while +100 ms does.
    CT_CHECK(ccord_sandbox_tick(sb, t0 + 10u, &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(hits == 0)", &err) == CCORD_OK);
    CT_CHECK(ccord_sandbox_tick(sb, t0 + 100u, &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(hits == 1)", &err) == CCORD_OK);
    // Backgrounding suspends periodic timers completely (no wakeup timer, no firing).
    ccord_sandbox_set_background(sb, true);
    CT_CHECK(ccord_sandbox_next_timer_ms(sb) == -1);
    CT_CHECK(ccord_sandbox_tick(sb, t0 + 5000u, &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(hits == 1)", &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);

    // Overdue periodic timer while backgrounded is re-armed cleanly on resume without an immediate burst.
    sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb, "hits = 0; time.every(1, function() hits = hits + 1 end)", &err) == CCORD_OK);
    ccord_sandbox_set_background(sb, true);
    ccord_sleep_ms(60);
    CT_CHECK(ccord_sandbox_next_timer_ms(sb) == -1);
    ccord_sandbox_set_background(sb, false);
    int next_ms = ccord_sandbox_next_timer_ms(sb);
    CT_CHECK(next_ms > 0 && next_ms <= 50);
    ccord_sandbox_destroy(sb);
  }

  // 6. Phase 1: local time/formatting, message_accessory position validation, rich events & jump_to_message.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_UI_DECORATE | CCORD_PERM_EVENTS_MESSAGES | CCORD_PERM_NAVIGATION_WRITE, 0, 0);
    CT_CHECK(run(sb,
                 "local off = time.tz_offset_min()\n"
                 "assert(type(off) == 'number' and off >= -720 and off <= 840)\n"
                 "local parts = time.local_parts(1700000000000)\n"
                 "assert(type(parts) == 'table' and parts.year >= 2023 and parts.month >= 1 and parts.month <= 12)\n"
                 "assert(parts.wday >= 1 and parts.wday <= 7 and type(parts.tz_offset_min) == 'number')\n"
                 "assert(time.format_discord(1700000000000, 'D', true) == 'November 14, 2023')\n"
                 "assert(time.format_discord(1700000000000, 'd', true) == '11/14/2023')\n"
                 "assert(#time.format_discord(1700000000000, 'T', true) > 0)\n"
                 "assert(#time.format_discord(1700000000000, 'R', true) > 0)\n"
                 "local ok_bad = pcall(time.format_discord, 1700000000000, 'bogus')\n"
                 "assert(not ok_bad)\n"
                 "assert(ui.decorate.message_accessory({id='acc1', message_id='100', position='above_body', priority=10, root={type='text', text='prev'}}))\n"
                 "assert(ui.decorate.message_accessory({id='acc2', message_id='100', root={type='text', text='below'}}))\n"
                 "local ok_pos = pcall(ui.decorate.message_accessory, {id='acc3', message_id='100', position='sideways', root={type='text', text='x'}})\n"
                 "assert(not ok_pos)\n"
                 "events.on('messages.loaded', function() end)\n"
                 "events.on('message.delete_bulk', function() end)\n"
                 "navigation.jump_to_message('200', '999')\n",
                 &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "navigation.jump_to_message") == 0 &&
             strstr(f.params, "\"channel_id\":\"200\"") != nullptr &&
             strstr(f.params, "\"message_id\":\"999\"") != nullptr);
    ccord_sandbox_destroy(sb);
  }

  // 7. Phase 2: plugin.on_unload LIFO + app.quit + bounded wall-clock abort + settings.on_action & inline button action.
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb,
                 "log_seq = {}\n"
                 "plugin.on_unload(function(r) log_seq[#log_seq + 1] = 'u1:' .. r end)\n"
                 "plugin.on_unload(function(r) log_seq[#log_seq + 1] = 'u2:' .. r end)\n"
                 "events.on('app.quit', function(ev) log_seq[#log_seq + 1] = 'quit:' .. ev.reason end)\n",
                 &err) == CCORD_OK);
    // "disable" runs LIFO on_unload only (NOT app.quit).
    CT_CHECK(ccord_sandbox_unload(sb, "disable", 200u, &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(table.concat(log_seq, ',') == 'u2:disable,u1:disable')", &err) == CCORD_OK);
    // "quit" runs LIFO on_unload AND app.quit.
    CT_CHECK(run(sb, "log_seq = {}", &err) == CCORD_OK);
    CT_CHECK(ccord_sandbox_unload(sb, "quit", 200u, &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(table.concat(log_seq, ',') == 'u2:quit,u1:quit,quit:quit')", &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);

    // Runaway infinite loop inside plugin.on_unload must abort cleanly within budget_ms.
    sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb, "plugin.on_unload(function() while true do end end)", &err) == CCORD_OK);
    uint64_t t0 = ccord_clock_ms();
    CT_CHECK(ccord_sandbox_unload(sb, "quit", 80u, &err) == CCORD_OK);
    uint64_t dt = ccord_clock_ms() - t0;
    CT_CHECK(dt >= 50u && dt < 1000u);
    ccord_sandbox_destroy(sb);

    // Settings schema inline action + settings.on_action both fire under user-gesture context.
    sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb,
                 "act_log = {}\n"
                 "settings.schema({\n"
                 "  { type = 'button', key = 'clear_cache', label = 'Cache', button_label = 'Clear', variant = 'danger',\n"
                 "    action = function(k) act_log[#act_log + 1] = 'inline:' .. k end },\n"
                 "})\n"
                 "settings.on_action(function(k) act_log[#act_log + 1] = 'global:' .. k end)\n",
                 &err) == CCORD_OK);
    CT_CHECK(ccord_sandbox_setting_action(sb, "clear_cache", &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(table.concat(act_log, ',') == 'inline:clear_cache,global:clear_cache')", &err) == CCORD_OK);
    ccord_sandbox_destroy(sb);

    // storage.files.* requires Class 2 storage.files permission.
    sb = make(&f, 0, 0, 0);
    f.nreq = 0;
    CT_CHECK(run(sb, "storage.files.write('a.txt', 'hi')", &err) != CCORD_OK);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
    ccord_sandbox_destroy(sb);
  }

  // 8. Phase 3: store.presence / store.activities + ui.decorate.channel_style / member_style + content_override.
  {
    ccord_sandbox *sb = make(&f, CCORD_PERM_UI_DECORATE, 0, 0);
    f.nreq = 0;
    CT_CHECK(run(sb,
                 "ui.decorate.channel_style({ id = 'cs1', channel_id = '42', tint = '#22334433', hidden = true })\n"
                 "ui.decorate.member_style({ id = 'ms1', user_id = '99', accent = '#ff0000', dim = true })\n",
                 &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.decorate.set") == 0 &&
             strstr(f.params, "\"target\":\"member_style\"") != nullptr &&
             strstr(f.params, "\"user_id\":\"99\"") != nullptr);
    // store.presence / store.activities require store.read.
    CT_CHECK(run(sb, "store.presence('99')", &err) != CCORD_OK);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
    ccord_sandbox_destroy(sb);

    sb = make(&f, CCORD_PERM_STORE_READ, 0, 0);
    f.nreq = 0;
    CT_CHECK(run(sb, "store.activities('77')", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "store.activities") == 0 && strstr(f.params, "\"user_id\":\"77\"") != nullptr);
    ccord_sandbox_destroy(sb);
  }

  // 9. Phase 4: 2,000 ms gesture window, ui.toast.show opts + action, ui.media.open / open_user_profile / menu.popup,
  //    and hooks.on_before_upload + hooks.complete_upload_transform permission checks.
  {
    ccord_sandbox *sb = make(&f, 0, 0, 0);
    f.nreq = 0;
    CT_CHECK(run(sb,
                 "-- Outside user gesture: Class 1 popups/lightboxes/profiles MUST be refused gracefully.\n"
                 "local ok1, e1 = ui.media.open({ url = 'https://cdn.discordapp.com/a.png' })\n"
                 "assert(ok1 == false and e1 and e1:find('user%-gesture'))\n"
                 "local ok2, e2 = ui.open_user_profile('42')\n"
                 "assert(ok2 == false and e2 and e2:find('user%-gesture'))\n"
                 "local ok3, e3 = ui.menu.popup({ items = { { id = 'a', label = 'A' } } })\n"
                 "assert(ok3 == false and e3 and e3:find('user%-gesture'))\n"
                 "-- Interactive toast with options + action callback\n"
                 "toast_clicked = false\n"
                 "tid = ui.toast.show('Deleted', 'warn', { duration_ms = 6000, action_label = 'View', on_action = function()\n"
                 "  toast_clicked = true\n"
                 "  -- Clicking a toast action enters user-gesture context!\n"
                 "  local pok = ui.open_user_profile('55', '10')\n"
                 "  assert(pok == true)\n"
                 "end })\n"
                 "assert(type(tid) == 'number' and tid > 0)\n",
                 &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.toast.show") == 0 &&
             strstr(f.params, "\"action_label\":\"View\"") != nullptr &&
             strstr(f.params, "\"duration_ms\":6000") != nullptr);
    CT_CHECK(run(sb,
                 "picked_item = 'unset'\n"
                 "commands.register('pop', 'Popup', function()\n"
                 "  ui.menu.popup({ x = 100, y = 120, items = { { id = 'copy', label = 'Copy' }, { id = 'del', label = 'Delete', danger = true } },\n"
                 "    on_select = function(id) picked_item = tostring(id) end })\n"
                 "end)\n",
                 &err) == CCORD_OK);

    // Trigger toast action -> callback fires AND enters user-gesture so ui.open_user_profile succeeds!
    CT_CHECK(ccord_sandbox_toast_action(sb, 1u, &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(toast_clicked == true)", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.open_user_profile") == 0 && strstr(f.params, "\"user_id\":\"55\"") != nullptr);

    // Within 2,000 ms post-gesture window, ui.media.open(url) ALSO succeeds even outside the synchronous frame!
    f.method[0] = '\0';
    CT_CHECK(run(sb, "assert(ui.media.open({ url = 'https://cdn.discordapp.com/a.png', kind = 'image' }) == true)", &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "ui.media.open") == 0);

    // Command opens popup menu -> popup_menu_result dispatches item id.
    ccord_arena ar; ccord_arena_init(&ar, 256u);
    const ccord_json_limits lim = {8, 64, 1024u};
    const ccord_json *ctx = ccord_json_parse(&ar, "{\"channel_id\":\"10\"}", 19, &lim, &err);
    char *out_json = nullptr;
    CT_CHECK(ccord_sandbox_command_run_ex(sb, "pop", "", ctx, &out_json, &err) == CCORD_OK);
    ccord_free(out_json);
    CT_CHECK(strcmp(f.method, "ui.menu.popup") == 0 && strstr(f.params, "\"danger\":true") != nullptr);
    CT_CHECK(ccord_sandbox_popup_menu_result(sb, 1u, "del", &err) == CCORD_OK);
    CT_CHECK(run(sb, "assert(picked_item == 'del')", &err) == CCORD_OK);

    // Source perm enforcement on ui.media.open: plugin_data:// without storage.files is fatal!
    CT_CHECK(run(sb, "ui.media.open({ local_path = 'plugin_data://img.png' })", &err) != CCORD_OK);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
    ccord_arena_free(&ar);
    ccord_sandbox_destroy(sb);

    // hooks.on_before_upload requires messages.hook permission.
    sb = make(&f, 0, 0, 0);
    CT_CHECK(run(sb, "hooks.on_before_upload(function() end)", &err) != CCORD_OK);
    CT_CHECK(ccord_sandbox_limit_hit(sb) == CCORD_SB_LIMIT_UNDECLARED);
    ccord_sandbox_destroy(sb);

    // With messages.hook, hooks.on_before_upload + async upload_transform dispatch cleanly!
    sb = make(&f, CCORD_PERM_MESSAGES_HOOK, 0, 0);
    f.nreq = 0;
    CT_CHECK(run(sb,
                 "hooks.on_before_upload(function(ctx) return { files = { [1] = { spoiler = true } } } end,\n"
                 "  function(job) return { notice = 'done:' .. job.files[1].handle } end)\n",
                 &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "hooks.register") == 0 && strstr(f.params, "\"before_upload\"") != nullptr);
    ccord_arena ar2; ccord_arena_init(&ar2, 512u);
    const ccord_json_limits lim2 = {8, 64, 1024u};
    const char *job_str = "{\"job_id\":7,\"files\":[{\"index\":1,\"handle\":\"upload_tmp://abc123\"}]}";
    const ccord_json *job_j = ccord_json_parse(&ar2, job_str, strlen(job_str), &lim2, &err);
    f.method[0] = '\0';
    CT_CHECK(ccord_sandbox_upload_transform(sb, job_j, &err) == CCORD_OK);
    CT_CHECK(strcmp(f.method, "hooks.complete_upload_transform") == 0 &&
             strstr(f.params, "\"job_id\":7") != nullptr &&
             strstr(f.params, "done:upload_tmp://abc123") != nullptr);
    ccord_arena_free(&ar2);
    ccord_sandbox_destroy(sb);
  }

  return CT_RESULT("test_host_sandbox");
}
