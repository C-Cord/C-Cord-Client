// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
// Integration test for Plugin API v2: manifest platforms/requires_capabilities filtering, Class 1 `platform.*`
// OS/capability detection, mobile UX APIs (`ui.haptics.*`, `ui.share.*`, `ui.keyboard.*`, `ui.mobile.*`),
// live platform/lifecycle events, and Android's in-process worker-thread Lua plugin host mode.
#define _POSIX_C_SOURCE 200809L
#include <ccord/core/alloc.h>
#include <ccord/core/event_bus.h>
#include <ccord/model/contrib.h>
#include <ccord/plugins/manifest.h>
#include <ccord/plugins/plugins.h>
#include <ccord/store/store.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ccord_test.h"
#include <ccord/plugin_host/host.h>

#define WAIT_MS 10000u

typedef struct mob_ops_state {
  int haptic_impact_calls;
  int last_haptic_style;
  int haptic_notify_calls;
  int last_haptic_kind;
  int share_calls;
  char last_share_title[128];
  char last_share_text[256];
  int kb_dismiss_calls;
  int keep_screen_calls;
  bool last_keep_screen;
  int orient_calls;
  int last_orient;
  int clip_calls;
  char last_clip[512];
} mob_ops_state;

static bool op_haptic_impact(void *ud, int style)
{
  mob_ops_state *s = ud;
  s->haptic_impact_calls++;
  s->last_haptic_style = style;
  return true;
}

static bool op_haptic_notify(void *ud, int kind)
{
  mob_ops_state *s = ud;
  s->haptic_notify_calls++;
  s->last_haptic_kind = kind;
  return true;
}

static bool op_share_text(void *ud, const char *title, const char *text)
{
  mob_ops_state *s = ud;
  s->share_calls++;
  snprintf(s->last_share_title, sizeof s->last_share_title, "%s", title ? title : "");
  snprintf(s->last_share_text, sizeof s->last_share_text, "%s", text ? text : "");
  return true;
}

static void op_kb_dismiss(void *ud)
{
  mob_ops_state *s = ud;
  s->kb_dismiss_calls++;
}

static bool op_keep_screen_on(void *ud, bool on)
{
  mob_ops_state *s = ud;
  s->keep_screen_calls++;
  s->last_keep_screen = on;
  return true;
}

static bool op_set_orientation(void *ud, int mode)
{
  mob_ops_state *s = ud;
  s->orient_calls++;
  s->last_orient = mode;
  return true;
}

static void on_clip(void *ud, const char *utf8)
{
  mob_ops_state *s = ud;
  s->clip_calls++;
  snprintf(s->last_clip, sizeof s->last_clip, "%s", utf8 ? utf8 : "");
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

static bool pump_until_state(ccord_plugins *pm, ccord_bus *bus, const ccord_contrib *c,
                             const char *id, ccord_plugin_state want)
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

static bool pump_until_clip(ccord_plugins *pm, ccord_bus *bus, const mob_ops_state *ops, const char *needle)
{
  uint64_t deadline = now_ms() + WAIT_MS;
  while (now_ms() < deadline) {
    ccord_plugins_pump(pm, now_ms());
    ccord_bus_drain(bus);
    if (strstr(ops->last_clip, needle) != nullptr) return true;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 5 * 1000 * 1000};
    (void)nanosleep(&ts, nullptr);
  }
  return false;
}

static void test_manifest_platform_and_caps(void)
{
  ccord_manifest m = {0};
  ccord_error err = {0};
  char why[160] = "";

  // 1. Omitted platforms defaults to CCORD_PLAT_ALL and zero required_caps.
  const char *j_default =
      "{\"schema\":1,\"id\":\"dev.test.any\",\"name\":\"Any\",\"version\":\"1.0.0\",\"api\":1,\"main\":\"main.lua\"}";
  CT_CHECK(ccord_manifest_parse(j_default, strlen(j_default), &m, &err) == CCORD_OK);
  CT_CHECK(m.platforms == CCORD_PLAT_ALL);
  CT_CHECK(m.required_caps == 0);
  CT_CHECK(ccord_manifest_supports(&m, CCORD_PLAT_ANDROID, 0, why, sizeof why));
  CT_CHECK(ccord_manifest_supports(&m, CCORD_PLAT_LINUX, 0, why, sizeof why));

  // 2. Android-only API v1 plugin with haptics + share requirement.
  const char *j_android =
      "{\"schema\":1,\"id\":\"dev.test.mob\",\"name\":\"Mob\",\"version\":\"1.0.0\",\"api\":1,"
      "\"main\":\"main.lua\",\"platforms\":[\"android\"],\"requires_capabilities\":[\"haptics\",\"share\"]}";
  CT_CHECK(ccord_manifest_parse(j_android, strlen(j_android), &m, &err) == CCORD_OK);
  CT_CHECK(m.api_version == 1);
  CT_CHECK(m.platforms == CCORD_PLAT_ANDROID);
  CT_CHECK((m.required_caps & (CCORD_CAP_HAPTICS | CCORD_CAP_SHARE)) == (CCORD_CAP_HAPTICS | CCORD_CAP_SHARE));
  CT_CHECK(!ccord_manifest_supports(&m, CCORD_PLAT_LINUX, CCORD_CAP_HAPTICS | CCORD_CAP_SHARE, why, sizeof why));
  CT_CHECK(strstr(why, "platform") != nullptr);
  CT_CHECK(!ccord_manifest_supports(&m, CCORD_PLAT_ANDROID, CCORD_CAP_HAPTICS, why, sizeof why));
  CT_CHECK(strstr(why, "share") != nullptr);
  CT_CHECK(ccord_manifest_supports(&m, CCORD_PLAT_ANDROID, CCORD_CAP_HAPTICS | CCORD_CAP_SHARE, why, sizeof why));

  // 3. Desktop-only plugin rejected on Android, accepted on Linux/macOS/Windows.
  const char *j_desk =
      "{\"schema\":1,\"id\":\"dev.test.desk\",\"name\":\"Desk\",\"version\":\"1.0.0\",\"api\":1,"
      "\"main\":\"main.lua\",\"platforms\":[\"desktop\"]}";
  CT_CHECK(ccord_manifest_parse(j_desk, strlen(j_desk), &m, &err) == CCORD_OK);
  CT_CHECK(m.platforms == CCORD_PLAT_DESKTOP);
  CT_CHECK(!ccord_manifest_supports(&m, CCORD_PLAT_ANDROID, 0, why, sizeof why));
  CT_CHECK(ccord_manifest_supports(&m, CCORD_PLAT_LINUX, 0, why, sizeof why));
  CT_CHECK(ccord_manifest_supports(&m, CCORD_PLAT_WINDOWS, 0, why, sizeof why));
  CT_CHECK(ccord_manifest_supports(&m, CCORD_PLAT_MACOS, 0, why, sizeof why));

  // 4. Unknown platform, unknown capability, or unsupported API version fails strict manifest parse.
  const char *j_bad_plat =
      "{\"schema\":1,\"id\":\"dev.test.bad\",\"name\":\"Bad\",\"version\":\"1.0.0\",\"api\":1,"
      "\"main\":\"main.lua\",\"platforms\":[\"templeos\"]}";
  CT_CHECK(ccord_manifest_parse(j_bad_plat, strlen(j_bad_plat), &m, &err) == CCORD_ERR_INVALID);

  const char *j_bad_cap =
      "{\"schema\":1,\"id\":\"dev.test.bad\",\"name\":\"Bad\",\"version\":\"1.0.0\",\"api\":1,"
      "\"main\":\"main.lua\",\"requires_capabilities\":[\"telepathy\"]}";
  CT_CHECK(ccord_manifest_parse(j_bad_cap, strlen(j_bad_cap), &m, &err) == CCORD_ERR_INVALID);

  const char *j_bad_api =
      "{\"schema\":1,\"id\":\"dev.test.bad\",\"name\":\"Bad\",\"version\":\"1.0.0\",\"api\":2,\"main\":\"main.lua\"}";
  CT_CHECK(ccord_manifest_parse(j_bad_api, strlen(j_bad_api), &m, &err) == CCORD_ERR_UNSUPPORTED);
}

static void test_in_process_android_plugin_host(void)
{
  char root[] = "/tmp/ccord-mob-api-XXXXXX";
  CT_CHECK(mkdtemp(root) != nullptr);
  char plugins_dir[1100], data_dir[1100], p_mob[1200], p_desk[1200], p_tray[1200];
  snprintf(plugins_dir, sizeof plugins_dir, "%s/plugins", root);
  snprintf(data_dir, sizeof data_dir, "%s/data", root);
  snprintf(p_mob, sizeof p_mob, "%s/mob_probe", plugins_dir);
  snprintf(p_desk, sizeof p_desk, "%s/desk_only", plugins_dir);
  snprintf(p_tray, sizeof p_tray, "%s/needs_tray", plugins_dir);
  CT_CHECK(mkdir(plugins_dir, 0700) == 0);
  CT_CHECK(mkdir(p_mob, 0700) == 0);
  CT_CHECK(mkdir(p_desk, 0700) == 0);
  CT_CHECK(mkdir(p_tray, 0700) == 0);

  // Desktop-only plugin -> skipped on simulated Android host.
  write_file(p_desk, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.deskonly\",\"name\":\"DeskOnly\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"platforms\":[\"desktop\"]}");
  write_file(p_desk, "main.lua", "assert(false, 'desktop-only plugin must not execute on android')\n");

  // Capability-gated plugin requiring tray -> skipped when tray capability is absent.
  write_file(p_tray, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.needstray\",\"name\":\"NeedsTray\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"requires_capabilities\":[\"tray\"]}");
  write_file(p_tray, "main.lua", "assert(false, 'tray plugin must not execute without tray capability')\n");

  // Cross-platform Class 1 plugin exercising platform.*, mobile APIs, and live events (ZERO permissions needed!).
  write_file(p_mob, "plugin.json",
             "{\"schema\":1,\"id\":\"dev.test.mobprobe\",\"name\":\"MobProbe\",\"version\":\"1.0.0\",\"api\":1,"
             "\"main\":\"main.lua\",\"platforms\":[\"android\",\"mobile\"],"
             "\"requires_capabilities\":[\"haptics\",\"share\"],\"permissions\":[]}");
  write_file(p_mob, "main.lua",
             "assert(platform.os == 'android')\n"
             "assert(platform.arch == 'arm64')\n"
             "assert(platform.form_factor == 'phone')\n"
             "assert(platform.is_mobile == true and platform.is_desktop == false)\n"
             "assert(platform.is_android == true and platform.is_linux == false)\n"
             "assert(platform.touch == true)\n"
             "assert(platform.api_version == 1)\n"
             "assert(platform.android_api_level == 35)\n"
             "assert(platform.has('haptics') == true)\n"
             "assert(platform.has('share') == true)\n"
             "assert(platform.has('tray') == false)\n"
             "assert(platform.capabilities.haptics == true and platform.capabilities.tray == false)\n"
             "local sa = platform.safe_area()\n"
             "assert(sa.top == 44 and sa.bottom == 34)\n"
             "local vp = platform.viewport()\n"
             "assert(vp.width == 412 and vp.height == 915)\n"
             "assert(platform.orientation() == 'portrait')\n"
             "local kb = platform.keyboard()\n"
             "assert(kb.visible == false and kb.height == 0)\n"
             "-- Invoke Class 1 mobile UX APIs\n"
             "assert(ui.haptics.impact('medium') == true)\n"
             "assert(ui.haptics.notify('success') == true)\n"
             "assert(ui.share.text('Hello Android', 'C-Cord Share') == true)\n"
             "assert(ui.share.url('https://ccord.example/invite', 'Invite') == true)\n"
             "ui.keyboard.dismiss()\n"
             "assert(ui.mobile.keep_screen_on(true) == true)\n"
             "assert(ui.mobile.set_orientation('landscape') == true)\n"
             "local seen = {orient = '', kb = '', sa = '', life = ''}\n"
             "local function report()\n"
             "  clipboard.write(seen.orient .. '|' .. seen.kb .. '|' .. seen.sa .. '|' .. seen.life)\n"
             "end\n"
             "events.on('platform.orientation', function(ev)\n"
             "  seen.orient = (ev.orientation or '') .. ':' .. platform.orientation()\n"
             "  report()\n"
             "end)\n"
             "events.on('platform.keyboard', function(ev)\n"
             "  seen.kb = tostring(ev.visible) .. ':' .. tostring(math.floor(ev.height or 0))\n"
             "  report()\n"
             "end)\n"
             "events.on('platform.safe_area', function(ev)\n"
             "  seen.sa = tostring(math.floor(ev.top or 0)) .. ':' .. tostring(math.floor(platform.safe_area().top))\n"
             "  report()\n"
             "end)\n"
             "events.on('app.lifecycle', function(ev)\n"
             "  seen.life = ev.state or ''\n"
             "  report()\n"
             "end)\n"
             "clipboard.write('init-ok')\n");

  ccord_error err = {0};
  ccord_bus *bus = ccord_bus_create(&err);
  CT_CHECK(bus != nullptr);
  ccord_store *store = ccord_store_create(bus, &err);
  CT_CHECK(store != nullptr);
  ccord_contrib *contrib = ccord_contrib_create(&err);
  CT_CHECK(contrib != nullptr);

  mob_ops_state ops = {0};
  const char *dirs[] = {plugins_dir};
  uint64_t caps = CCORD_CAP_TOUCH | CCORD_CAP_HAPTICS | CCORD_CAP_SHARE | CCORD_CAP_SAFE_AREA |
                  CCORD_CAP_KEYBOARD_CONTROL | CCORD_CAP_ORIENTATION_CONTROL | CCORD_CAP_KEEP_SCREEN_ON |
                  CCORD_CAP_PANELS;
  ccord_plugins_config cfg = {
    .in_process = true,
    .in_process_host_fn = ccord_plugin_host_run_fd,
    .search_dirs = dirs,
    .search_dir_count = 1,
    .data_dir = data_dir,
    .bus = bus,
    .store = store,
    .contrib = contrib,
    .clipboard_write = on_clip,
    .clipboard_ud = &ops,
    .has_platform_info = true,
    .platform_info = {
      .os = "android",
      .arch = "arm64",
      .form_factor = "phone",
      .is_mobile = true,
      .touch = true,
      .android_api_level = 35,
      .capabilities = caps,
    },
    .platform_state = {
      .safe_top = 44.0f,
      .safe_bottom = 34.0f,
      .viewport_w = 412.0f,
      .viewport_h = 915.0f,
      .viewport_scale = 2.625f,
      .keyboard_height = 0.0f,
      .keyboard_visible = false,
      .landscape = false,
    },
    .platform_ops = {
      .ud = &ops,
      .haptic_impact = op_haptic_impact,
      .haptic_notify = op_haptic_notify,
      .share_text = op_share_text,
      .keyboard_dismiss = op_kb_dismiss,
      .keep_screen_on = op_keep_screen_on,
      .set_orientation = op_set_orientation,
    },
  };

  ccord_plugins *pm = ccord_plugins_create(&cfg, &err);
  CT_CHECK(pm != nullptr);
  size_t started = ccord_plugins_load_all(pm);
  CT_CHECK(started == 1);

  // Verify incompatible plugins were skipped cleanly with human reasons.
  const ccord_plugin_info *desk_info = find_info(contrib, "dev.test.deskonly");
  CT_CHECK(desk_info != nullptr && desk_info->state == CCORD_PLUGIN_FAILED);
  CT_CHECK(desk_info && desk_info->error && strstr(desk_info->error, "platform") != nullptr);

  const ccord_plugin_info *tray_info = find_info(contrib, "dev.test.needstray");
  CT_CHECK(tray_info != nullptr && tray_info->state == CCORD_PLUGIN_FAILED);
  CT_CHECK(tray_info && tray_info->error && strstr(tray_info->error, "tray") != nullptr);

  // Verify in-process mobile plugin loaded and ran every mobile API callback.
  CT_CHECK(pump_until_state(pm, bus, contrib, "dev.test.mobprobe", CCORD_PLUGIN_READY));
  CT_CHECK(pump_until_clip(pm, bus, &ops, "init-ok"));
  CT_CHECK(ops.haptic_impact_calls == 1 && ops.last_haptic_style == 1);
  CT_CHECK(ops.haptic_notify_calls == 1 && ops.last_haptic_kind == 0);
  CT_CHECK(ops.share_calls == 2);
  CT_CHECK(strcmp(ops.last_share_title, "Invite") == 0);
  CT_CHECK(strcmp(ops.last_share_text, "https://ccord.example/invite") == 0);
  CT_CHECK(ops.kb_dismiss_calls == 1);
  CT_CHECK(ops.keep_screen_calls == 1 && ops.last_keep_screen == true);
  CT_CHECK(ops.orient_calls == 1 && ops.last_orient == 2);

  // Push live platform state change (landscape + keyboard open + updated safe top).
  ccord_plugin_platform_state next_st = {
    .safe_top = 48.0f,
    .safe_bottom = 24.0f,
    .viewport_w = 915.0f,
    .viewport_h = 412.0f,
    .viewport_scale = 2.625f,
    .keyboard_height = 280.0f,
    .keyboard_visible = true,
    .landscape = true,
  };
  ccord_plugins_update_platform_state(pm, &next_st);
  ccord_plugins_dispatch_lifecycle(pm, "background");
  CT_CHECK(pump_until_clip(pm, bus, &ops, "landscape:landscape|true:280|48:48|background"));

  ccord_plugins_dispatch_lifecycle(pm, "low_memory");
  CT_CHECK(pump_until_clip(pm, bus, &ops, "|low_memory"));

  ccord_plugins_shutdown(pm);
  ccord_plugins_destroy(pm);
  ccord_contrib_destroy(contrib);
  ccord_store_destroy(store);
  ccord_bus_destroy(bus);

  // Cleanup temp files.
  char path[1400];
  const char *files[] = {"mob_probe/plugin.json", "mob_probe/main.lua",
                         "desk_only/plugin.json", "desk_only/main.lua",
                         "needs_tray/plugin.json", "needs_tray/main.lua"};
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", plugins_dir, files[i]);
    (void)unlink(path);
  }
  snprintf(path, sizeof path, "%s/plugins/dev.test.mobprobe/storage.json", data_dir);
  (void)unlink(path);
  snprintf(path, sizeof path, "%s/plugins/dev.test.mobprobe", data_dir);
  (void)rmdir(path);
  snprintf(path, sizeof path, "%s/plugins", data_dir);
  (void)rmdir(path);
  snprintf(path, sizeof path, "%s/plugins.json", data_dir);
  (void)unlink(path);
  (void)rmdir(data_dir);
  (void)rmdir(p_mob);
  (void)rmdir(p_desk);
  (void)rmdir(p_tray);
  (void)rmdir(plugins_dir);
  (void)rmdir(root);
}

int main(void)
{
  test_manifest_platform_and_caps();
  test_in_process_android_plugin_host();
  return CT_RESULT("test_plugins_mobile_api");
}
