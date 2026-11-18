// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/alloc.h>
#include <sys/stat.h>
#include <ccord/core/event_bus.h>
#include <ccord/model/contrib.h>
#include <ccord/plugins/plugins.h>
#include <ccord/store/store.h>

#include <signal.h>
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
} ui_hook_state;

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
    .ud = &uih, .navigate = it_ui_nav, .ui_action = it_ui_action, .get_view = it_ui_view
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

  // Export .cce and re-import it.
  char cce_path[1400], ebuf[256] = "";
  snprintf(cce_path, sizeof cce_path, "%s/kept.cce", data_dir);
  CT_CHECK(ccord_contrib_plugin_export_cce(contrib, "dev.test.kept", cce_path, ebuf, sizeof ebuf));
  CT_CHECK(ccord_contrib_plugin_import_package(contrib, cce_path, ebuf, sizeof ebuf));

  // Copy Message ID: loads, registers its menu item, and writes the message id through the host.
  CT_CHECK(pump_until(pm, bus, contrib, "dev.ccord.copy-message-id", CCORD_PLUGIN_READY));
  bool menu_found = false;
  for (size_t i = 0; i < ccord_contrib_menu_count(contrib, CCORD_MENU_MESSAGE); i++) {
    const ccord_menu_contribution *item = ccord_contrib_menu_at(contrib, CCORD_MENU_MESSAGE, i);
    if (strcmp(item->item_id, "copy-message-id") == 0 && strcmp(item->label, "Copy Message ID") == 0) menu_found = true;
  }
  CT_CHECK(menu_found);

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
  ccord_plugins_shutdown(pm);
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
  return CT_RESULT("test_plugins_integration");
}
