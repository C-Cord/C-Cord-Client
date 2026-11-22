// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/alloc.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ccord/core/event_bus.h>
#include <ccord/model/contrib.h>
#include <ccord/plugins/plugins.h>
#include <ccord/store/store.h>

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ccord_test.h"

#define WAIT_MS 15000u

typedef struct clip_state {
  int calls;
  char text[128];
} clip_state;

typedef struct ui_hook_state {
  char view[32];
  char last_action[32];
  char last_draft[128];
  ccord_id last_draft_ch;
} ui_hook_state;

static bool it_ui_insert_draft(void *ud, ccord_id ch, const char *text)
{
  ui_hook_state *st = ud;
  st->last_draft_ch = ch;
  snprintf(st->last_draft, sizeof st->last_draft, "%s", text ? text : "");
  return true;
}

static bool it_ui_nav(void *ud, ccord_id g, ccord_id c, ccord_id p, const char *name, const char *view)
{
  ui_hook_state *st = ud;
  (void)g; (void)c; (void)p; (void)name;
  if (view && view[0]) {
    snprintf(st->view, sizeof st->view, "%s", view);
    return true;
  }
  return false;
}

static bool it_ui_action(void *ud, const char *action)
{
  ui_hook_state *st = ud;
  if (!action) return false;
  snprintf(st->last_action, sizeof st->last_action, "%s", action);
  return true;
}

static const char *it_ui_view(void *ud)
{
  const ui_hook_state *st = ud;
  return st->view[0] ? st->view : "channel";
}

static void on_clipboard(void *ud, const char *utf8)
{
  clip_state *c = ud;
  c->calls++;
  snprintf(c->text, sizeof c->text, "%s", utf8);
}

static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void write_file(const char *dir, const char *name, const char *body)
{
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  CT_CHECK(f != nullptr);
  if (f) {
    CT_CHECK(fputs(body, f) >= 0);
    fclose(f);
  }
}

static const ccord_plugin_info *find_info(const ccord_contrib *c, const char *id)
{
  for (size_t i = 0; i < ccord_contrib_plugin_count(c); i++) {
    const ccord_plugin_info *info = ccord_contrib_plugin_at(c, i);
    if (strcmp(info->id, id) == 0) return info;
  }
  return nullptr;
}

// Pumps plugins and the bus until `done` holds or the deadline passes.
static bool pump_until(ccord_plugins *pm, ccord_bus *bus, const ccord_contrib *c, const char *id, ccord_plugin_state want)
{
  uint64_t deadline = now_ms() + WAIT_MS;
  while (now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    ccord_bus_drain(bus);
    const ccord_plugin_info *info = find_info(c, id);
    if (info && info->state == want) return true;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  return false;
}

static bool error_has(const ccord_contrib *c, const char *id, const char *needle)
{
  const ccord_plugin_info *info = find_info(c, id);
  return info && info->error && strstr(info->error, needle) != nullptr;
}

typedef struct v3_clip_log {
  char items[64][256];
  size_t count;
} v3_clip_log;

typedef struct v3_comp_state {
  char text[256];
  ccord_id reply_id;
  bool reply_ping;
  char attached_name[64];
  size_t attached_len;
} v3_comp_state;

typedef struct v3_local_srv {
  int listen_fd;
  uint16_t port;
  _Atomic bool stop;
} v3_local_srv;

static void v3_on_clip(void *ud, const char *utf8)
{
  v3_clip_log *cl = ud;
  if (cl->count < 64u) snprintf(cl->items[cl->count++], 256u, "%s", utf8 ? utf8 : "");
}

static bool v3_has_clip(const v3_clip_log *cl, const char *needle)
{
  for (size_t i = 0; i < cl->count; i++) {
    if (strstr(cl->items[i], needle) != nullptr) return true;
  }
  return false;
}

static bool v3_pump_clip(ccord_plugins *pm, ccord_bus *bus, const v3_clip_log *cl, const char *needle)
{
  uint64_t deadline = now_ms() + WAIT_MS;
  while (now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    ccord_bus_drain(bus);
    if (v3_has_clip(cl, needle)) return true;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  return false;
}

static bool v3_comp_get(void *ud, char *out, size_t cap, size_t *s0, size_t *s1, ccord_id *rid, bool *rp)
{
  v3_comp_state *cs = ud;
  if (out && cap) snprintf(out, cap, "%s", cs->text);
  if (s0) *s0 = 0;
  if (s1) *s1 = strlen(cs->text);
  if (rid) *rid = cs->reply_id;
  if (rp) *rp = cs->reply_ping;
  return true;
}

static bool v3_comp_set(void *ud, const char *text, bool ins, int pos)
{
  (void)pos;
  v3_comp_state *cs = ud;
  if (!text) text = "";
  if (ins) {
    size_t cur = strlen(cs->text);
    snprintf(cs->text + cur, sizeof cs->text - cur, "%s", text);
  } else {
    snprintf(cs->text, sizeof cs->text, "%s", text);
  }
  return true;
}

static bool v3_comp_reply(void *ud, ccord_id mid, bool ping)
{
  v3_comp_state *cs = ud;
  cs->reply_id = mid;
  cs->reply_ping = ping;
  return true;
}

static bool v3_comp_attach(void *ud, const char *fname, const uint8_t *data, size_t len, bool spoiler)
{
  (void)data;
  (void)spoiler;
  v3_comp_state *cs = ud;
  snprintf(cs->attached_name, sizeof cs->attached_name, "%s", fname ? fname : "");
  cs->attached_len = len;
  return true;
}

static void *v3_srv_thread(void *arg)
{
  v3_local_srv *ctx = arg;
  struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
  (void)setsockopt(ctx->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  int handled = 0;
  while (!atomic_load(&ctx->stop) && handled < 4) {
    int cfd = accept(ctx->listen_fd, nullptr, nullptr);
    if (cfd < 0) continue;
    handled++;
    struct timeval ctv = {.tv_sec = 5, .tv_usec = 0};
    (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof ctv);
    char req[2048] = "";
    ssize_t rn = recv(cfd, req, sizeof req - 1, 0);
    if (rn > 0) {
      req[rn] = '\0';
      if (strstr(req, "Upgrade: websocket") != nullptr) {
        const char *hs =
          "HTTP/1.1 101 Switching Protocols\r\n"
          "Upgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
        (void)send(cfd, hs, strlen(hs), 0);
        uint8_t fh[2] = {0};
        if (recv(cfd, fh, 2, MSG_WAITALL) == 2) {
          bool masked = (fh[1] & 0x80u) != 0;
          size_t plen = (size_t)(fh[1] & 0x7fu);
          uint8_t mk[4] = {0};
          if (masked) (void)recv(cfd, mk, 4, MSG_WAITALL);
          uint8_t pay[128] = {0};
          if (plen < sizeof pay && recv(cfd, pay, plen, MSG_WAITALL) == (ssize_t)plen) {
            if (masked) { for (size_t i = 0; i < plen; i++) pay[i] ^= mk[i & 3u]; }
            uint8_t resp[160];
            char msg[140];
            int mn = snprintf(msg, sizeof msg, "echo:%.*s", (int)plen, (const char *)pay);
            resp[0] = 0x81u;
            resp[1] = (uint8_t)mn;
            memcpy(resp + 2, msg, (size_t)mn);
            (void)send(cfd, resp, (size_t)mn + 2u, 0);
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 80 * 1000 * 1000};
            (void)nanosleep(&ts, nullptr);
          }
        }
      } else {
        const char *http_resp =
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 10\r\nConnection: close\r\n\r\npong-local";
        (void)send(cfd, http_resp, strlen(http_resp), 0);
      }
    }
    close(cfd);
  }
  return nullptr;
}

static void test_api_v3_integration(void)
{
  char root[] = "/tmp/ccord-it-v3-XXXXXX";
  CT_CHECK(mkdtemp(root) != nullptr);
  char plugins_dir[1100], data_dir[1100], watch_dir[1100];
  char p1[1200], p2[1200], p3[1200];
  snprintf(plugins_dir, sizeof plugins_dir, "%s/plugins", root);
  snprintf(data_dir, sizeof data_dir, "%s/data", root);
  snprintf(watch_dir, sizeof watch_dir, "%s/watch", root);
  snprintf(p1, sizeof p1, "%s/v3c1", plugins_dir);
  snprintf(p2, sizeof p2, "%s/v3c2", plugins_dir);
  snprintf(p3, sizeof p3, "%s/v3c3", plugins_dir);
  CT_CHECK(mkdir(plugins_dir, 0700) == 0);
  CT_CHECK(mkdir(watch_dir, 0700) == 0);
  CT_CHECK(mkdir(p1, 0700) == 0);
  CT_CHECK(mkdir(p2, 0700) == 0);
  CT_CHECK(mkdir(p3, 0700) == 0);

  // Start local loopback HTTP + WebSocket server thread.
  v3_local_srv srv = {.listen_fd = -1};
  srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  CT_CHECK(srv.listen_fd >= 0);
  int one = 1;
  (void)setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
  CT_CHECK(bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof addr) == 0);
  socklen_t alen = sizeof addr;
  CT_CHECK(getsockname(srv.listen_fd, (struct sockaddr *)&addr, &alen) == 0);
  srv.port = ntohs(addr.sin_port);
  CT_CHECK(listen(srv.listen_fd, 8) == 0);
  pthread_t srv_tid;
  CT_CHECK(pthread_create(&srv_tid, nullptr, v3_srv_thread, &srv) == 0);

  // Set test env vars (one normal, one secret-blocked).
  setenv("CCORD_TEST_V3_ENV", "hello-env-v3", 1);
  setenv("DISCORD_TOKEN", "should-never-leak", 1);

  // Plugin 1: Class 1 v3 features + RPC provider + anti-escalation caller.
  write_file(p1, "note.txt", "asset-ok-v3");
  write_file(p1, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.v3c1\",\"name\":\"V3C1\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"permissions\":[\"commands\",\"storage\",\"clipboard.write\"]}");
  write_file(p1, "main.lua",
             "local a = plugin.read_asset('note.txt')\n"
             "clipboard.write('c1_asset:' .. tostring(a))\n"
             "settings.schema({{key='flag', type='toggle', label='Flag', default=false}})\n"
             "settings.on_change(function(k, v) clipboard.write('c1_set:' .. k .. '=' .. tostring(v)) end)\n"
             "theme.register({id='cyber', name='Cyber', base='c-cord-dark', css=':root{--brand-experiment:#00ffcc;}'})\n"
             "ui.actionButton.add({id='hdr1', slot='header', label='V3', action=function() clipboard.write('c1_btn:clicked') end})\n"
             "keybinds.register({id='kb1', label='Test KB', default='CTRL+SHIFT+Y', action=function() clipboard.write('c1_kb:fired') end})\n"
             "commands.register({\n"
             "  name='v3cmd', description='V3 test',\n"
             "  autocomplete=function(args) return {{label='opt_'..args, detail='d', insert_text='opt_'..args}} end,\n"
             "  run=function(args)\n"
             "    if args == 'esc' then\n"
             "      plugin.call('dev.test.v3c3', 'c3_secret', function(res, err)\n"
             "        clipboard.write(res ~= nil and 'c1_esc:LEAKED' or ('c1_esc:DENIED:' .. tostring(err)))\n"
             "      end)\n"
             "      return\n"
             "    end\n"
             "    return 'ran:' .. args\n"
             "  end\n"
             "})\n"
             "plugin.expose('c1_echo', function(s) return 'echoed:' .. tostring(s) end)\n");

  // Plugin 2: Class 2 v3 features (panel patch, decorators, theme, composer, hooks, gateway, local msg, earcon, RPC).
  write_file(p2, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.v3c2\",\"name\":\"V3C2\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"permissions\":[\"ui.panel\",\"ui.decorate\",\"ui.composer\","
             "\"theme.write\",\"messages.hook\",\"notifications.hook\",\"gateway.raw\","
             "\"audio.earcons\",\"clipboard.write\",\"commands\"]}");
  write_file(p2, "main.lua",
             "ui.panel.mount({id='p2', slot='modal', width=360, height=200, title='P2',\n"
             "  root={type='column', id='r', children={{type='text', id='t1', text='init'}}}})\n"
             "ui.panel.patch('p2', {{op='set_text', id='t1', value='patched_v3'}})\n"
             "ui.decorate.message_badge({id='b1', target_id='500', widget={type='badge', id='bb', text='VIP'},\n"
             "  on_event=function(ev, tid) clipboard.write('c2_decor:' .. tostring(tid) .. ':' .. tostring(ev.id)) end})\n"
             "ui.decorate.message_style({id='s1', target_id='500', accent='#ff00aa', tint='#11223344', collapsed=true, collapse_label='Folded'})\n"
             "theme.inject_css('l1', '.chat { background-color: #101218; }')\n"
             "theme.set_icon('mic', 'v3c2:mic')\n"
             "ui.composer.set_text('draft_v3')\n"
             "ui.composer.insert('_plus')\n"
             "ui.composer.set_reply('999', false)\n"
             "ui.composer.attach_bytes('hello.txt', 'abcde', true)\n"
             "local ct = ui.composer.get_text()\n"
             "clipboard.write('c2_comp:' .. tostring(ct))\n"
             "ui.composer.register_autocomplete({id='sn', trigger='::', description='Snip',\n"
             "  complete=function(q) return {{label='snip_'..q, detail='snip', insert_text='SNIP_'..q}} end})\n"
             "hooks.on_before_send(function(d)\n"
             "  if d.content == 'cancelme' then return {cancel=true, reason='blocked_v3'} end\n"
             "  if d.content == 'hello' then return {content='HELLO_V3!'} end\n"
             "end)\n"
             "hooks.on_link_open(function(d)\n"
             "  if d.url == 'https://x.com/test' then return {url='https://fxtwitter.com/test'} end\n"
             "end)\n"
             "hooks.on_notification(function(d)\n"
             "  if d.content == 'mute-me' then return {suppress=true} end\n"
             "  return {title='V3:' .. (d.title or '')}\n"
             "end)\n"
             "gateway.on_raw('TEST_V3_EVENT', function(d) clipboard.write('c2_gw:' .. tostring(d.val)) end)\n"
             "messages.inject_local('200', {author='V3Bot', badge='BOT', content='Ephemeral hello', dismissible=true})\n"
             "audio.override_earcon('mention', 'RIFFxxxx')\n"
             "commands.register('callc1', 'RPC test', function()\n"
             "  plugin.call('dev.test.v3c1', 'c1_echo', 'world', function(res, err)\n"
             "    clipboard.write('c2_rpc:' .. tostring(res) .. ':' .. tostring(err))\n"
             "  end)\n"
             "end)\n");

  // Plugin 3: Class 3 v3 features (fs.stat, fs.watch, system.exec, system.spawn, system.env, net.local, net.ws, RPC).
  write_file(p3, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.v3c3\",\"name\":\"V3C3\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"permissions\":[\"fs.read\",\"fs.watch\",\"system.exec\","
             "\"system.env\",\"network.local\",\"clipboard.write\"]}");
  char p3_lua[4096];
  snprintf(p3_lua, sizeof p3_lua,
           "local st = fs.stat('%s')\n"
           "clipboard.write('c3_stat:' .. tostring(st and st.is_dir))\n"
           "fs.watch('%s', function(ev) clipboard.write('c3_watch:' .. tostring(ev.kind)) end)\n"
           "local okv = system.env('CCORD_TEST_V3_ENV')\n"
           "local tok = system.env('DISCORD_TOKEN')\n"
           "clipboard.write('c3_env:' .. tostring(okv) .. ':' .. tostring(tok))\n"
           "system.exec({cmd='/bin/sh', args={'-c', 'printf exec_ok_v3'}}, function(r, e)\n"
           "  clipboard.write('c3_exec:' .. tostring(r and r.code) .. ':' .. tostring(r and r.stdout))\n"
           "end)\n"
           "local sp_out = ''\n"
           "system.spawn({cmd='/bin/sh', args={'-c', 'printf spawn_ok_v3'},\n"
           "  on_stdout=function(d) sp_out = sp_out .. (d.data or '') end,\n"
           "  on_exit=function(d) clipboard.write('c3_spawn:' .. tostring(d.code) .. ':' .. sp_out) end})\n"
           "net.local_net.request({url='http://127.0.0.1:%u/ping', method='GET'}, function(r, e)\n"
           "  clipboard.write('c3_http:' .. tostring(r and r.status) .. ':' .. tostring(r and r.body))\n"
           "end)\n"
           "local ws = net.ws.connect('ws://127.0.0.1:%u/ws', {\n"
           "  on_open=function() end,\n"
           "  on_message=function(m) clipboard.write('c3_ws:' .. tostring(m.data)) end,\n"
           "})\n"
           "ws:on_open(function() ws:send('ping_ws') end)\n"
           "plugin.expose('c3_secret', function() return 'TOP_SECRET' end)\n",
           watch_dir, watch_dir, (unsigned)srv.port, (unsigned)srv.port);
  write_file(p3, "main.lua", p3_lua);

  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  ccord_store *store = ccord_store_create(bus, &err);
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(bus && store && contrib);

  v3_clip_log cl = {0};
  v3_comp_state comp = {0};
  ccord_contrib_ui_hooks uih = {
    .ud = &comp,
    .composer_get = v3_comp_get,
    .composer_set = v3_comp_set,
    .composer_set_reply = v3_comp_reply,
    .composer_attach_bytes = v3_comp_attach,
  };
  ccord_contrib_set_ui_hooks(contrib, &uih);

  const char *dirs[] = {plugins_dir};
  ccord_plugins_config cfg = {
    .store = store, .contrib = contrib, .bus = bus,
    .search_dirs = dirs, .search_dir_count = 1,
    .data_dir = data_dir, .host_exe = CCORD_PLUGIN_HOST_BIN,
    .clipboard_write = v3_on_clip, .clipboard_ud = &cl,
  };
  ccord_plugins *pm = ccord_plugins_create(&cfg, &err);
  CT_CHECK(pm != nullptr);
  (void)ccord_plugins_load_all(pm);

  // Class 1 loads immediately; Class 2 & Class 3 wait for consent.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.v3c1", CCORD_PLUGIN_READY));
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.v3c2", CCORD_PLUGIN_NEEDS_CONSENT));
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.v3c3", CCORD_PLUGIN_NEEDS_CONSENT));

  const ccord_plugin_info *i2 = find_info(contrib, "dev.test.v3c2");
  const ccord_plugin_info *i3 = find_info(contrib, "dev.test.v3c3");
  CT_CHECK(i2 && i3);
  ccord_contrib_plugin_consent_accept_for(contrib, "dev.test.v3c2", i2->perms_mask);
  ccord_contrib_plugin_consent_accept_for(contrib, "dev.test.v3c3", i3->perms_mask);
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.v3c2", CCORD_PLUGIN_READY));
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.v3c3", CCORD_PLUGIN_READY));

  // Verify Class 1 v3 startup contributions & callbacks.
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c1_asset:asset-ok-v3"));
  CT_CHECK(ccord_contrib_find_theme_preset(contrib, "dev.test.v3c1", "cyber") != nullptr);
  CT_CHECK(ccord_contrib_plugin_settings(contrib, "dev.test.v3c1") != nullptr);
  ccord_contrib_notify_setting_changed(contrib, "dev.test.v3c1", "flag", "true");
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c1_set:flag=true"));
  ccord_menu_context act_ctx = {.target = CCORD_MENU_CHANNEL, .channel_id = 200};
  CT_CHECK(ccord_contrib_click_action_btn(contrib, "dev.test.v3c1", "hdr1", &act_ctx));
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c1_btn:clicked"));
  CT_CHECK(ccord_contrib_trigger_keybind(contrib, "dev.test.v3c1", "kb1"));
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c1_kb:fired"));
  ccord_autocomplete_item cmd_items[4];
  size_t ncmd = ccord_contrib_complete_command(contrib, "v3cmd", "abc", 100, 200, cmd_items, 4);
  CT_CHECK(ncmd == 1 && strcmp(cmd_items[0].label, "opt_abc") == 0);

  // Verify Class 2 v3 features: composer, panel patch, decorators, theme, hooks, gateway, local msg, earcon.
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c2_comp:draft_v3_plus"));
  CT_CHECK(comp.reply_id == 999 && !comp.reply_ping);
  CT_CHECK(strcmp(comp.attached_name, "hello.txt") == 0 && comp.attached_len == 5);
  const ccord_panel_contribution *p2pan = ccord_contrib_find_panel(contrib, "dev.test.v3c2", "p2");
  CT_CHECK(p2pan && strstr(p2pan->root_json, "patched_v3") != nullptr);
  CT_CHECK(ccord_contrib_theme_layer_count(contrib) >= 1);
  CT_CHECK(ccord_contrib_find_icon_override(contrib, "mic") != nullptr);
  CT_CHECK(ccord_contrib_local_message_count(contrib, 200) == 1);
  CT_CHECK(ccord_contrib_find_earcon_override(contrib, "mention") != nullptr);
  ccord_panel_event dev = {.kind = CCORD_PANEL_EV_CLICK, .node_id = "bb"};
  CT_CHECK(ccord_contrib_dispatch_decor_event(contrib, "dev.test.v3c2", "b1", 500, &dev, &err) == CCORD_OK);
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c2_decor:500:bb"));
  ccord_autocomplete_item ac_items[4];
  size_t nac = ccord_contrib_query_autocomplete(contrib, "dev.test.v3c2", "sn", "hi", 100, 200, ac_items, 4);
  CT_CHECK(nac == 1 && strcmp(ac_items[0].insert_text, "SNIP_hi") == 0);

  // Synchronous interception hooks (before_send rewrite & cancel, link_open rewrite, notification rewrite & suppress).
  char out_txt[256] = "", reason[128] = "";
  CT_CHECK(ccord_contrib_run_before_send(contrib, 200, 0, "hello", out_txt, sizeof out_txt, reason, sizeof reason));
  CT_CHECK(strcmp(out_txt, "HELLO_V3!") == 0);
  CT_CHECK(!ccord_contrib_run_before_send(contrib, 200, 0, "cancelme", out_txt, sizeof out_txt, reason, sizeof reason));
  CT_CHECK(strcmp(reason, "blocked_v3") == 0);
  char out_url[256] = "";
  CT_CHECK(ccord_contrib_run_link_open_hook(contrib, "https://x.com/test", out_url, sizeof out_url));
  CT_CHECK(strcmp(out_url, "https://fxtwitter.com/test") == 0);
  char nt[128] = "Ping", nb[128] = "hi";
  bool silent = false;
  CT_CHECK(ccord_contrib_run_notification_hook(contrib, 100, 200, 300, 400, "hello", true, false, nt, sizeof nt, nb, sizeof nb, &silent));
  CT_CHECK(strcmp(nt, "V3:Ping") == 0);
  CT_CHECK(!ccord_contrib_run_notification_hook(contrib, 100, 200, 300, 400, "mute-me", true, false, nt, sizeof nt, nb, sizeof nb, &silent));

  // Raw gateway event dispatch: subscriber query must report active subscription before shutdown and false after.
  CT_CHECK(ccord_plugins_has_raw_gateway_subscribers(pm));
  ccord_plugins_dispatch_raw_gateway(pm, "TEST_V3_EVENT", "{\"val\":42}");
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c2_gw:42"));

  // Inter-plugin RPC: Class 2 -> Class 1 succeeds; Class 1 -> Class 3 blocked (anti-escalation)!
  CT_CHECK(ccord_contrib_run_command(contrib, "callc1", "", 100, 200, &err) == CCORD_OK);
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c2_rpc:echoed:world:nil"));
  CT_CHECK(ccord_contrib_run_command(contrib, "v3cmd", "esc", 100, 200, &err) == CCORD_OK);
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c1_esc:DENIED:"));
  CT_CHECK(!v3_has_clip(&cl, "c1_esc:LEAKED"));

  // Class 3 v3 features: fs.stat, fs.watch, system.env (with secret block), system.exec, system.spawn, net.local HTTP & WS.
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c3_stat:true"));
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c3_env:hello-env-v3:nil"));
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c3_exec:0:exec_ok_v3"));
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c3_spawn:0:spawn_ok_v3"));
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c3_http:200:pong-local"));
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c3_ws:echo:ping_ws"));

  // Trigger fs.watch by writing a file inside watch_dir.
  write_file(watch_dir, "touched.txt", "change");
  CT_CHECK(v3_pump_clip(pm, bus, &cl, "c3_watch:"));

  atomic_store(&srv.stop, true);
  (void)pthread_join(srv_tid, nullptr);
  close(srv.listen_fd);

  ccord_plugins_shutdown(pm);
  CT_CHECK(!ccord_plugins_has_raw_gateway_subscribers(pm));
  ccord_plugins_destroy(pm);
  ccord_contrib_destroy(contrib);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);

  // Cleanup temp tree.
  char rm[1400];
  snprintf(rm, sizeof rm, "%s/touched.txt", watch_dir); (void)unlink(rm);
  (void)rmdir(watch_dir);
  const char *fdel[] = {
    "v3c1/note.txt", "v3c1/plugin.json", "v3c1/main.lua",
    "v3c2/plugin.json", "v3c2/main.lua",
    "v3c3/plugin.json", "v3c3/main.lua",
  };
  for (size_t i = 0; i < sizeof fdel / sizeof fdel[0]; i++) {
    snprintf(rm, sizeof rm, "%s/%s", plugins_dir, fdel[i]);
    (void)unlink(rm);
  }
  (void)rmdir(p1); (void)rmdir(p2); (void)rmdir(p3); (void)rmdir(plugins_dir);
  snprintf(rm, sizeof rm, "%s/plugins/dev.test.v3c1/storage.json", data_dir); (void)unlink(rm);
  snprintf(rm, sizeof rm, "%s/plugins/dev.test.v3c1", data_dir); (void)rmdir(rm);
  snprintf(rm, sizeof rm, "%s/plugins", data_dir); (void)rmdir(rm);
  snprintf(rm, sizeof rm, "%s/plugins.json", data_dir); (void)unlink(rm);
  (void)rmdir(data_dir);
  (void)rmdir(root);
}

int main(void)
{
  char root[] = "/tmp/ccord-it-XXXXXX";
  if (!mkdtemp(root)) return 1;
  char plugins_dir[1100], denied[1200], loop[1200], hog[1200], data_dir[1100];
  snprintf(plugins_dir, sizeof plugins_dir, "%s/plugins", root);
  snprintf(data_dir, sizeof data_dir, "%s/data", root);
  snprintf(denied, sizeof denied, "%s/denied", plugins_dir);
  snprintf(loop, sizeof loop, "%s/loop", plugins_dir);
  snprintf(hog, sizeof hog, "%s/hog", plugins_dir);
  CT_CHECK(mkdir(plugins_dir, 0700) == 0);
  CT_CHECK(mkdir(denied, 0700) == 0);
  CT_CHECK(mkdir(loop, 0700) == 0);
  CT_CHECK(mkdir(hog, 0700) == 0);
  char kept[1200], pnav[1200];
  snprintf(kept, sizeof kept, "%s/kept", plugins_dir);
  snprintf(pnav, sizeof pnav, "%s/pnav", plugins_dir);
  CT_CHECK(mkdir(kept, 0700) == 0);
  CT_CHECK(mkdir(pnav, 0700) == 0);
  write_file(kept, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.kept\",\"name\":\"Kept\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"permissions\":[\"storage\",\"clipboard.write\"]}");
  write_file(kept, "main.lua",
             "local prev = storage.get('items')\n"
             "if prev and type(prev) == 'table' and #prev == 2 and prev[2].id == 'b' then\n"
             "  clipboard.write('reloaded:' .. prev[1].id .. ':' .. tostring(prev[2].n))\n"
             "else\n"
             "  storage.set('greeting', 'hi')\n"
             "  storage.set('items', {{id = 'a', n = 1}, {id = 'b', n = 2}})\n"
             "end\n");

  write_file(pnav, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.panel\",\"name\":\"PanelNav\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"permissions\":[\"ui.panel\",\"ui.contextMenu\",\"store.read\","
             "\"navigation.write\",\"events.navigation\",\"clipboard.write\"]}");
  write_file(pnav, "main.lua",
             "ui.panel.mount({\n"
             "  id = 'bar', slot = 'top', height = 34,\n"
             "  root = {type = 'row', children = {{type = 'button', id = 'btn-friends', label = 'Friends'}}},\n"
             "  on_event = function(ev)\n"
             "    if ev.type == 'click' and ev.id == 'btn-friends' then\n"
             "      navigation.select_view('friends')\n"
             "      navigation.action('quick_switcher')\n"
             "      ui.panel.update('bar', {height = 38, root = {type = 'row', children = {}}})\n"
             "      local sel = store.selected()\n"
             "      local dm = store.guild('dm')\n"
             "      clipboard.write((sel and sel.view or '?') .. ':' .. (dm and dm.name or '?'))\n"
             "    elseif ev.type == 'middle_click' and ev.id == 'unmount' then\n"
             "      ui.panel.unmount('bar')\n"
             "    end\n"
             "  end\n"
             "})\n"
             "events.on('navigation.aux_open', function(ev)\n"
             "  clipboard.write('aux:' .. (ev.view or '') .. ':' .. (ev.channel_id or ''))\n"
             "end)\n");

  // Undeclared Class 2 call is ignored and logged (plugin still loads); malformed Lua fails with debug log;
  // Class 2/3 plugin waits for consent until accepted.
  write_file(denied, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.denied\",\"name\":\"Denied\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"permissions\":[]}");
  write_file(denied, "main.lua", "pcall(function() messages.send('1', 'x') end)\n");
  write_file(loop, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.syntax\",\"name\":\"Syntax\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\"}");
  write_file(loop, "main.lua", "this is not valid lua !!\n");
  write_file(hog, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.consent\",\"name\":\"Consent\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"permissions\":[\"messages.send\"]}");
  write_file(hog, "main.lua", "log.info('consented!')\n");

  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  CT_CHECK(bus != nullptr);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(store != nullptr);
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);
  clip_state clip = {0};
  ui_hook_state uih = {.view = "channel"};
  ccord_contrib_ui_hooks hooks = {
    .ud = &uih, .navigate = it_ui_nav, .ui_action = it_ui_action, .get_view = it_ui_view, .insert_draft = it_ui_insert_draft
  };
  ccord_contrib_set_ui_hooks(contrib, &hooks);

  // Seed store with a channel so navigation.aux_open enrichment resolves channel info.
  ccord_user self_u = {.id = 1, .name = "Tester", .status_text = "", .presence = CCORD_PRESENCE_ONLINE};
  ccord_guild g1 = {.id = 100, .name = "Dev Server"};
  ccord_channel c1 = {.id = 200, .guild_id = 100, .kind = CCORD_CHANNEL_TEXT, .name = "general", .topic = "hi"};
  ccord_snapshot snap = {.self_id = 1, .users = &self_u, .user_count = 1, .guilds = &g1, .guild_count = 1,
                         .channels = &c1, .channel_count = 1};
  CT_CHECK(ccord_store_set_snapshot(store, &snap, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_guild(store, 100, &err) == CCORD_OK);
  CT_CHECK(ccord_store_select_channel(store, 200, &err) == CCORD_OK);

  // The real extension directory is the second search path; it must load Copy Message ID unchanged.
  const char *search[] = {plugins_dir, CCORD_SOURCE_DIR "/extensions"};
  ccord_plugins_config cfg = {
    .host_exe = CCORD_PLUGIN_HOST_BIN,
    .search_dirs = search, .search_dir_count = 2,
    .data_dir = data_dir,
    .bus = bus, .store = store, .session = nullptr, .contrib = contrib,
    .clipboard_write = on_clipboard, .clipboard_ud = &clip,
  };
  ccord_plugins *pm = ccord_plugins_create(&cfg, &err);
  CT_CHECK(pm != nullptr);
  CT_CHECK(ccord_plugins_load_all(pm) == 5);

  // Undeclared API call: even inside pcall, host exits immediately and main marks plugin FAILED + logs it.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.denied", CCORD_PLUGIN_FAILED));
  CT_CHECK(ccord_contrib_plugin_error_count(contrib, "dev.test.denied") >= 1);

  // Malformed Lua: init fails, main records FAILED with Lua error text and debug log entry.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.syntax", CCORD_PLUGIN_FAILED));
  CT_CHECK(ccord_contrib_plugin_error_count(contrib, "dev.test.syntax") >= 1);

  // Class 2 plugin: starts in NEEDS_CONSENT, then becomes READY after user accepts consent.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.consent", CCORD_PLUGIN_NEEDS_CONSENT));
  ccord_contrib_plugin_consent_accept(contrib, "dev.test.consent");
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.consent", CCORD_PLUGIN_READY));

  // Export .cce and re-import it. Wait for dev.test.kept before and after so its initial storage write
  // completes before reload and its reload clipboard write ("reloaded:a:2") cannot race later checks.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.kept", CCORD_PLUGIN_READY));
  char cce_path[1400], ebuf[256] = "";
  snprintf(cce_path, sizeof cce_path, "%s/kept.cce", data_dir);
  CT_CHECK(ccord_contrib_plugin_export_cce(contrib, "dev.test.kept", cce_path, ebuf, sizeof ebuf));
  CT_CHECK(ccord_contrib_plugin_import_package(contrib, cce_path, ebuf, sizeof ebuf));
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.kept", CCORD_PLUGIN_READY));

  // Copy Message ID: loads, registers its menu item, and writes the message id through the host.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.ccord.copy-message-id", CCORD_PLUGIN_READY));
  bool menu_found = false;
  for (size_t i = 0; i < ccord_contrib_menu_count(contrib, CCORD_MENU_MESSAGE); i++) {
    const ccord_menu_contribution *item = ccord_contrib_menu_at(contrib, CCORD_MENU_MESSAGE, i);
    if (strcmp(item->item_id, "copy-message-id") == 0 && strcmp(item->label, "Copy Message ID") == 0) menu_found = true;
  }
  CT_CHECK(menu_found);

  clip.calls = 0;
  clip.text[0] = '\0';
  ccord_menu_context ctx = {.target = CCORD_MENU_MESSAGE, .guild_id = 1, .channel_id = 2, .message_id = 4242, .user_id = 7};
  CT_CHECK(ccord_contrib_activate_menu(contrib, "dev.ccord.copy-message-id", "copy-message-id", &ctx, &err) == CCORD_OK);
  uint64_t deadline = now_ms() + WAIT_MS;
  while (clip.calls == 0 && now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  CT_CHECK(clip.calls == 1);
  CT_CHECK(strcmp(clip.text, "4242") == 0);

  // kill -9 of a running host: main marks it FAILED, drops its menu items, and keeps the client alive.
  int pid = ccord_plugins_host_pid(pm, "dev.ccord.copy-message-id");
  CT_CHECK(pid > 0);
  if (pid > 0) CT_CHECK(kill((pid_t)pid, SIGKILL) == 0);
  CT_CHECK(pump_until(pm, bus, contrib, "dev.ccord.copy-message-id", CCORD_PLUGIN_FAILED));
  CT_CHECK(error_has(contrib, "dev.ccord.copy-message-id", "host killed by signal 9"));
  CT_CHECK(ccord_contrib_menu_count(contrib, CCORD_MENU_MESSAGE) == 0);

  // End-to-end UI panel + navigation + store + aux_open IPC round-trip.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.panel", CCORD_PLUGIN_NEEDS_CONSENT));
  ccord_contrib_plugin_consent_accept(contrib, "dev.test.panel");
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.panel", CCORD_PLUGIN_READY));
  deadline = now_ms() + WAIT_MS;
  while (!ccord_contrib_find_panel(contrib, "dev.test.panel", "bar") && now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  const ccord_panel_contribution *pbar = ccord_contrib_find_panel(contrib, "dev.test.panel", "bar");
  CT_CHECK(pbar != nullptr && pbar->slot == CCORD_PANEL_TOP && pbar->height == 34.0f);

  clip.calls = 0;
  clip.text[0] = '\0';
  ccord_panel_event pev = {.kind = CCORD_PANEL_EV_CLICK, .node_id = "btn-friends", .button = 1};
  CT_CHECK(ccord_contrib_dispatch_panel_event(contrib, "dev.test.panel", "bar", &pev, &err) == CCORD_OK);
  deadline = now_ms() + WAIT_MS;
  while (clip.calls == 0 && now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  CT_CHECK(clip.calls == 1 && strcmp(clip.text, "friends:Direct Messages") == 0);
  CT_CHECK(strcmp(uih.view, "friends") == 0 && strcmp(uih.last_action, "quick_switcher") == 0);
  pbar = ccord_contrib_find_panel(contrib, "dev.test.panel", "bar");
  CT_CHECK(pbar != nullptr && pbar->height == 38.0f);

  // Emit navigation.aux_open on the bus and verify the plugin receives it with resolved channel_id + view.
  clip.calls = 0;
  clip.text[0] = '\0';
  CT_CHECK(ccord_bus_post(bus, (ccord_event){.type = CCORD_EVT_NAV_AUX, .guild_id = 100, .channel_id = 200,
                                             .value = CCORD_NAV_VIEW_CHANNEL}) == CCORD_OK);
  ccord_bus_drain(bus);
  deadline = now_ms() + WAIT_MS;
  while (clip.calls == 0 && now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  CT_CHECK(clip.calls == 1 && strcmp(clip.text, "aux:channel:200") == 0);

  // Unmount via panel event.
  ccord_panel_event uev = {.kind = CCORD_PANEL_EV_MIDDLE_CLICK, .node_id = "unmount", .button = 2};
  CT_CHECK(ccord_contrib_dispatch_panel_event(contrib, "dev.test.panel", "bar", &uev, &err) == CCORD_OK);
  deadline = now_ms() + WAIT_MS;
  while (ccord_contrib_find_panel(contrib, "dev.test.panel", "bar") && now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  CT_CHECK(ccord_contrib_find_panel(contrib, "dev.test.panel", "bar") == nullptr);

  // Storage: a value (including nested tables/arrays) reaches <data>/plugins/<id>/storage.json on shutdown
  // and reloads cleanly into Lua on the next startup.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.test.kept", CCORD_PLUGIN_READY));
  ccord_plugins_shutdown(pm);
  char saved_path[1400];
  snprintf(saved_path, sizeof saved_path, "%s/plugins/dev.test.kept/storage.json", data_dir);
  char saved[512] = "";
  FILE *sf = fopen(saved_path, "r");
  CT_CHECK(sf != nullptr);
  if (sf) {
    size_t got = fread(saved, 1, sizeof saved - 1, sf);
    saved[got] = '\0';
    fclose(sf);
  }
  CT_CHECK(strstr(saved, "\"greeting\":\"hi\"") != nullptr);
  CT_CHECK(strstr(saved, "\"items\":[") != nullptr);

  ccord_plugins_destroy(pm);

  // Restart plugin manager to verify nested table/array storage reloads cleanly on startup.
  clip.calls = 0;
  clip.text[0] = '\0';
  const char *kept_only[] = {plugins_dir};
  ccord_plugins_config cfg2 = cfg;
  cfg2.search_dirs = kept_only;
  cfg2.search_dir_count = 1;
  pm = ccord_plugins_create(&cfg2, &err);
  CT_CHECK(pm != nullptr);
  (void)ccord_plugins_load_all(pm);
  deadline = now_ms() + WAIT_MS;
  while (clip.calls == 0 && now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    ccord_bus_drain(bus);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  CT_CHECK(clip.calls == 1 && strcmp(clip.text, "reloaded:a:2") == 0);

  // Security regressions (P4, P5, B14):
  // - Built-in slash command reservation + Class 1 command populates draft instead of auto-sending (P5).
  // - Brokered fs.read denies protected host config/data paths (P4).
  // - Uninstall + re-import runs cleanly without UAF (B14).
  {
    char sec_dir[1200];
    snprintf(sec_dir, sizeof sec_dir, "%s/sec", plugins_dir);
    CT_CHECK(mkdir(sec_dir, 0700) == 0);
    write_file(sec_dir, "plugin.json",
               "{\"schema\":1,\"id\":\"dev.test.sec\",\"name\":\"Sec\",\"version\":\"1.0.0\",\"api\":1,"
               "\"main\":\"main.lua\",\"permissions\":[\"commands\",\"fs.read\",\"clipboard.write\"]}");
    char sec_lua[2048];
    snprintf(sec_lua, sizeof sec_lua,
             "commands.register('tts', 'hijack', function() return 'bad' end)\n"
             "commands.register('secping', 'Draft test', function(args) return 'draft:' .. args end)\n"
             "local data, rerr = fs.read('%s/plugins.json')\n"
             "clipboard.write(data ~= nil and 'fs:LEAKED' or 'fs:BLOCKED')\n",
             data_dir);
    write_file(sec_dir, "main.lua", sec_lua);
    char sec_cce[1400], sebuf[256] = "";
    snprintf(sec_cce, sizeof sec_cce, "%s/sec.cce", data_dir);
    ccord_plugins_shutdown(pm);
    ccord_plugins_destroy(pm);

    clip.calls = 0;
    clip.text[0] = '\0';
    uih.last_draft[0] = '\0';
    uih.last_draft_ch = CCORD_ID_NONE;
    pm = ccord_plugins_create(&cfg2, &err);
    CT_CHECK(pm != nullptr);
    (void)ccord_plugins_load_all(pm);
    // Let pre-existing dev.test.kept finish its startup clipboard write before consenting dev.test.sec.
    CT_CHECK(pump_until(pm, bus, contrib, "dev.test.kept", CCORD_PLUGIN_READY));
    CT_CHECK(pump_until(pm, bus, contrib, "dev.test.sec", CCORD_PLUGIN_NEEDS_CONSENT));
    const ccord_plugin_info *sinfo = find_info(contrib, "dev.test.sec");
    CT_CHECK(sinfo != nullptr && sinfo->perms_mask != 0);
    // Accepting with a mismatched mask must NOT grant consent (P13).
    ccord_contrib_plugin_consent_accept_for(contrib, "dev.test.sec", 0);
    ccord_plugins_pump(pm, now_ms());
    sinfo = find_info(contrib, "dev.test.sec");
    CT_CHECK(sinfo && sinfo->state == CCORD_PLUGIN_NEEDS_CONSENT);
    // Accepting with the exact mask enables the plugin.
    clip.calls = 0;
    clip.text[0] = '\0';
    ccord_contrib_plugin_consent_accept_for(contrib, "dev.test.sec", sinfo->perms_mask);
    CT_CHECK(pump_until(pm, bus, contrib, "dev.test.sec", CCORD_PLUGIN_READY));
    deadline = now_ms() + WAIT_MS;
    while ((strncmp(clip.text, "fs:", 3) != 0 || !ccord_contrib_find_command(contrib, "secping")) && now_ms() < deadline) {
      ccord_plugins_pump(pm, now_ms());
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
      (void)nanosleep(&ts, nullptr);
    }
    // Protected host path read must have been blocked by the broker; reserved 'tts' command must not register.
    CT_CHECK(strcmp(clip.text, "fs:BLOCKED") == 0);
    CT_CHECK(ccord_contrib_find_command(contrib, "tts") == nullptr);
    CT_CHECK(ccord_contrib_find_command(contrib, "secping") != nullptr);

    // Invoking 'secping' without messages.send routes output to the composer draft hook.
    CT_CHECK(ccord_contrib_run_command(contrib, "secping", "hello", 100, 200, &err) == CCORD_OK);
    deadline = now_ms() + WAIT_MS;
    while (uih.last_draft[0] == '\0' && now_ms() < deadline) {
      ccord_plugins_pump(pm, now_ms());
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
      (void)nanosleep(&ts, nullptr);
    }
    CT_CHECK(uih.last_draft_ch == 200 && strcmp(uih.last_draft, "draft:hello") == 0);

    // Export, uninstall, and re-import without UAF (B14).
    CT_CHECK(ccord_contrib_plugin_export_cce(contrib, "dev.test.sec", sec_cce, sebuf, sizeof sebuf));
    CT_CHECK(ccord_contrib_plugin_uninstall(contrib, "dev.test.sec", sebuf, sizeof sebuf));
    CT_CHECK(find_info(contrib, "dev.test.sec") == nullptr);
    CT_CHECK(ccord_contrib_plugin_import_package(contrib, sec_cce, sebuf, sizeof sebuf));
    CT_CHECK(find_info(contrib, "dev.test.sec") != nullptr);
    (void)unlink(sec_cce);
    char sp[1400];
    snprintf(sp, sizeof sp, "%s/plugins/dev.test.sec/main.lua", data_dir); (void)unlink(sp);
    snprintf(sp, sizeof sp, "%s/plugins/dev.test.sec/plugin.json", data_dir); (void)unlink(sp);
    snprintf(sp, sizeof sp, "%s/plugins/dev.test.sec/storage.json", data_dir); (void)unlink(sp);
    snprintf(sp, sizeof sp, "%s/plugins/dev.test.sec", data_dir); (void)rmdir(sp);
    snprintf(sp, sizeof sp, "%s/main.lua", sec_dir); (void)unlink(sp);
    snprintf(sp, sizeof sp, "%s/plugin.json", sec_dir); (void)unlink(sp);
    (void)rmdir(sec_dir);
  }

  ccord_plugins_shutdown(pm);
  CT_CHECK(!ccord_plugins_has_raw_gateway_subscribers(pm));
  ccord_plugins_destroy(pm);
  ccord_contrib_destroy(contrib);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);

  // Remove everything the test created, deepest paths first.
  char path[1400];
  const char *files[] = {"denied/plugin.json", "denied/main.lua", "loop/plugin.json", "loop/main.lua",
                         "hog/plugin.json", "hog/main.lua", "kept/plugin.json", "kept/main.lua",
                         "pnav/plugin.json", "pnav/main.lua"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/plugins/%s", root, files[i]);
    (void)unlink(path);
  }
  (void)unlink(saved_path);
  (void)unlink(cce_path);
  snprintf(path, sizeof path, "%s/plugins/dev.test.kept/main.lua", data_dir);
  (void)unlink(path);
  snprintf(path, sizeof path, "%s/plugins/dev.test.kept/plugin.json", data_dir);
  (void)unlink(path);
  snprintf(path, sizeof path, "%s/plugins/dev.test.kept", data_dir);
  (void)rmdir(path);
  snprintf(path, sizeof path, "%s/plugins/dev.test.panel/storage.json", data_dir);
  (void)unlink(path);
  snprintf(path, sizeof path, "%s/plugins/dev.test.panel", data_dir);
  (void)rmdir(path);
  snprintf(path, sizeof path, "%s/plugins", data_dir);
  (void)rmdir(path);
  snprintf(path, sizeof path, "%s/plugins.json", data_dir);
  (void)unlink(path);
  (void)rmdir(data_dir);
  snprintf(path, sizeof path, "%s/kept", plugins_dir);
  (void)rmdir(path);
  (void)rmdir(pnav);
  (void)rmdir(denied);
  (void)rmdir(loop);
  (void)rmdir(hog);
  (void)rmdir(plugins_dir);
  (void)rmdir(root);
  test_api_v3_integration();
  return CT_RESULT("test_plugins_integration");
}
