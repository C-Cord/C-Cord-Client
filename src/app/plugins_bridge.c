// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "app_internal.h"

#include <ccord/assets/embedded_assets.h>
#include <ccord/core/alloc.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/json.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/net/http_fetch.h>
#if CCORD_WITH_PLUGINS
#include <ccord/plugin_host/host.h>
#endif

#if !CCORD_OS_WINDOWS
#include <dirent.h>
#include <unistd.h>
#endif
#include <SDL3/SDL_video.h>
#include <stdlib.h>
#include <string.h>

#if CCORD_WITH_PLUGINS
// Clipboard writes from extensions arrive here after the manager checked clipboard.write and the size limit.
static void plugin_clipboard(void *ud, const char *utf8)
{
  app_host_clipboard_set(ud, utf8);
}

// Starts the plugin manager: discovers extensions, spawns one host process each. Failures are logged and shown in Settings.
static bool catalog_http_get_adapter(void *ud, const char *url, const char *if_none_match, size_t max_bytes,
                                     char **out_body, size_t *out_len, char *out_etag, size_t etag_cap,
                                     bool *out_not_modified, char *err_buf, size_t err_cap)
{
  (void)ud;
  ccord_http_get_result r = {0};
  bool ok = ccord_http_get(url, if_none_match, max_bytes, &r);
  if (out_etag && etag_cap) snprintf(out_etag, etag_cap, "%s", r.etag);
  if (out_not_modified) *out_not_modified = r.not_modified;
  if (!ok) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", r.error);
    ccord_http_get_result_free(&r);
    return false;
  }
  if (out_body) *out_body = r.body;
  else ccord_http_get_result_free(&r);
  if (out_len) *out_len = r.body_len;
  return true;
}

static bool catalog_http_post_adapter(void *ud, const char *url, const char *json_body, size_t body_len,
                                      size_t max_bytes, char **out_body, size_t *out_len,
                                      char *err_buf, size_t err_cap)
{
  (void)ud;
  ccord_http_get_result r = {0};
  bool ok = ccord_http_post_json(url, json_body, body_len, max_bytes, &r);
  if (!ok) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", r.error);
    ccord_http_get_result_free(&r);
    return false;
  }
  if (out_body) *out_body = r.body;
  else ccord_http_get_result_free(&r);
  if (out_len) *out_len = r.body_len;
  return true;
}

static bool catalog_http_download_adapter(void *ud, const char *url, const char *dest_path, size_t max_bytes,
                                          char *err_buf, size_t err_cap)
{
  (void)ud;
  return ccord_http_download_file(url, dest_path, max_bytes, err_buf, err_cap);
}

static bool plugin_op_haptic_impact(void *ud, int style)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (style < 0 || style > CCORD_HAPTIC_SELECTION) style = CCORD_HAPTIC_LIGHT;
  return ccord_platform_haptic_impact(a->platform, (ccord_haptic_style)style);
}

static bool plugin_op_haptic_notify(void *ud, int kind)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (kind < 0 || kind > CCORD_HAPTIC_NOTIFY_ERROR) kind = CCORD_HAPTIC_NOTIFY_SUCCESS;
  return ccord_platform_haptic_notify(a->platform, (ccord_haptic_notify_kind)kind);
}

static bool plugin_op_share_text(void *ud, const char *title, const char *text)
{
  app *a = ud;
  return a && a->platform && ccord_platform_share_text(a->platform, title, text);
}

static void plugin_op_keyboard_dismiss(void *ud)
{
  app *a = ud;
  if (a && a->platform) ccord_platform_dismiss_keyboard(a->platform);
}

static bool plugin_op_keep_screen_on(void *ud, bool on)
{
  app *a = ud;
  return a && a->platform && ccord_platform_keep_screen_on(a->platform, on);
}

static bool plugin_op_set_orientation(void *ud, int mode)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  if (mode < 0 || mode > CCORD_ORIENT_LANDSCAPE) mode = CCORD_ORIENT_AUTO;
  return ccord_platform_set_orientation(a->platform, (ccord_orientation_mode)mode);
}

static bool plugin_op_open_url(void *ud, const char *url)
{
  (void)ud;
  return url && url[0] && ccord_platform_open_url(url);
}

static bool plugin_op_notify(void *ud, const char *title, const char *body, const char *tag, bool silent)
{
  (void)tag;
  (void)silent;
  app *a = ud;
  if (!a || !a->platform || !title) return false;
  return ccord_platform_notify(a->platform, title, body ? body : "");
}

static char *plugin_op_clipboard_read(void *ud)
{
  app *a = ud;
  return a && a->platform ? ccord_platform_clipboard_get(a->platform) : nullptr;
}

static bool plugin_op_clipboard_read_image(void *ud, uint8_t **out_png, size_t *out_len)
{
  return app_host_get_clipboard_image(ud, out_png, out_len);
}

static bool plugin_op_clipboard_write_image(void *ud, const uint8_t *png, size_t len)
{
  app *a = ud;
  if (!a || !a->platform) return false;
  ccord_error err = {0};
  return ccord_platform_set_clipboard_image(a->platform, png, len, &err) == CCORD_OK;
}

static bool plugin_op_http_request(void *ud, const ccord_plugin_http_req *req, ccord_plugin_http_resp *resp)
{
  (void)ud;
  if (!req || !resp) return false;
  ccord_http_request_opts opts = {
    .url = req->url,
    .method = req->method,
    .headers_json = req->headers_json,
    .body = req->body,
    .body_len = req->body_len,
    .timeout_ms = req->timeout_ms,
    .max_bytes = req->max_bytes,
    .allow_localhost_http = req->allow_localhost_http,
  };
  ccord_http_response hr = {0};
  bool ok = ccord_http_request(&opts, &hr);
  resp->status_code = hr.status_code;
  resp->body = hr.body;
  resp->body_len = hr.body_len;
  resp->headers_json = hr.headers_json;
  snprintf(resp->error, sizeof resp->error, "%s", hr.error);
  return ok;
}

static void *plugin_op_ws_open(void *ud, const char *url, const char *headers_json, char *err_buf, size_t err_cap)
{
  (void)ud;
  return ccord_ws_client_open(url, headers_json, err_buf, err_cap);
}

static bool plugin_op_ws_send(void *ud, void *handle, const uint8_t *data, size_t len, bool binary)
{
  (void)ud;
  return ccord_ws_client_send((ccord_ws_client *)handle, data, len, binary);
}

static int plugin_op_ws_recv(void *ud, void *handle, uint8_t *buf, size_t cap, bool *out_binary, int timeout_ms)
{
  (void)ud;
  return ccord_ws_client_recv((ccord_ws_client *)handle, buf, cap, out_binary, timeout_ms);
}

static void plugin_op_ws_close(void *ud, void *handle, uint16_t code, const char *reason)
{
  (void)ud;
  ccord_ws_client_close((ccord_ws_client *)handle, code, reason);
}

static char *plugin_op_theme_current_json(void *ud)
{
  app *a = ud;
  if (!a || !a->theme) return nullptr;
  ccord_buf b = {0};
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, a->theme->id);
  ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, a->theme->name);
  ccord_jw_key(&w, "colors");
  ccord_jw_begin_object(&w);
  size_t nc = ccord_theme_color_count();
  for (size_t i = 0; i < nc; i++) {
    const char *cname = ccord_theme_color_name(i);
    ccord_color col = {0};
    if (cname && ccord_theme_color_get(a->theme, cname, &col)) {
      char hex[16];
      if (col.a == 255) snprintf(hex, sizeof hex, "#%02x%02x%02x", col.r, col.g, col.b);
      else snprintf(hex, sizeof hex, "#%02x%02x%02x%02x", col.r, col.g, col.b, col.a);
      ccord_jw_key(&w, cname);
      ccord_jw_cstr(&w, hex);
    }
  }
  ccord_jw_end_object(&w);
  ccord_jw_key(&w, "metrics");
  ccord_jw_begin_object(&w);
  size_t nm = ccord_theme_metric_count();
  for (size_t i = 0; i < nm; i++) {
    const char *mname = ccord_theme_metric_name(i);
    float val = 0.0f;
    if (mname && ccord_theme_metric_get(a->theme, mname, &val)) {
      ccord_jw_key(&w, mname);
      ccord_jw_double(&w, (double)val);
    }
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  char *out = (b.data && ccord_jw_ok(&w)) ? ccord_strdup((const char *)b.data) : nullptr;
  ccord_buf_free(&b);
  return out;
}

static char *plugin_op_theme_color_hex(void *ud, const char *token)
{
  app *a = ud;
  ccord_color col = {0};
  if (!a || !a->theme || !token || !ccord_theme_color_get(a->theme, token, &col)) return nullptr;
  char hex[16];
  if (col.a == 255) snprintf(hex, sizeof hex, "#%02x%02x%02x", col.r, col.g, col.b);
  else snprintf(hex, sizeof hex, "#%02x%02x%02x%02x", col.r, col.g, col.b, col.a);
  return ccord_strdup(hex);
}

static bool plugin_op_theme_metric_val(void *ud, const char *metric, double *out_val)
{
  app *a = ud;
  float v = 0.0f;
  if (!a || !a->theme || !metric || !ccord_theme_metric_get(a->theme, metric, &v)) return false;
  if (out_val) *out_val = (double)v;
  return true;
}

static char *plugin_op_theme_list_json(void *ud)
{
  app *a = ud;
  ccord_buf b = {0};
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_array(&w);
  static const struct { const char *id, *name; } k_builtin[3] = {
    {"c-cord-dark", "Dark"}, {"c-cord-light", "Light"}, {"c-cord-amoled", "AMOLED"},
  };
  for (size_t i = 0; i < 3; i++) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, k_builtin[i].id);
    ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, k_builtin[i].name);
    ccord_jw_key(&w, "builtin"); ccord_jw_bool(&w, true);
    ccord_jw_end_object(&w);
  }
  if (a && a->contrib) {
    size_t np = ccord_contrib_theme_preset_count(a->contrib);
    for (size_t i = 0; i < np; i++) {
      const ccord_theme_preset *pr = ccord_contrib_theme_preset_at(a->contrib, i);
      if (!pr) continue;
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, pr->preset_id);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, pr->name);
      ccord_jw_key(&w, "owner"); ccord_jw_cstr(&w, pr->owner);
      ccord_jw_key(&w, "builtin"); ccord_jw_bool(&w, false);
      ccord_jw_end_object(&w);
    }
  }
  ccord_jw_end_array(&w);
  char *out = (b.data && ccord_jw_ok(&w)) ? ccord_strdup((const char *)b.data) : nullptr;
  ccord_buf_free(&b);
  return out;
}

static bool plugin_op_scheduled_add(void *ud, ccord_id channel_id, const char *utf8, size_t len,
                                    int64_t due_unix_ms, char *err_buf, size_t err_cap)
{
  return app_host_scheduled_add(ud, channel_id, utf8, len, due_unix_ms, ccord_clock_unix_ms(),
                            nullptr, nullptr, 0, err_buf, err_cap);
}

static char *plugin_op_scheduled_list_json(void *ud)
{
  (void)ud;
  ccord_ui_scheduled_item items[CCORD_UI_SCHEDULED_MAX];
  size_t n = ccord_scheduled_messages_list(items, CCORD_UI_SCHEDULED_MAX);
  ccord_buf b = {0};
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < n; i++) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_id(&w, items[i].id);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, items[i].channel_id);
    ccord_jw_key(&w, "due_unix_ms"); ccord_jw_int(&w, items[i].due_unix_ms);
    ccord_jw_key(&w, "preview"); ccord_jw_cstr(&w, items[i].preview);
    ccord_jw_key(&w, "missed"); ccord_jw_bool(&w, items[i].missed);
    ccord_jw_end_object(&w);
  }
  ccord_jw_end_array(&w);
  char *out = (b.data && ccord_jw_ok(&w)) ? ccord_strdup((const char *)b.data) : nullptr;
  ccord_buf_free(&b);
  return out;
}

static bool plugin_op_scheduled_cancel(void *ud, uint64_t id)
{
  return app_host_scheduled_cancel(ud, id);
}

static char *plugin_op_voice_levels_json(void *ud)
{
  app *a = ud;
  ccord_buf b = {0};
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  if (a && a->store) {
    ccord_id ch = ccord_store_active_voice_channel(a->store);
    if (ch != CCORD_ID_NONE) {
      size_t n = ccord_store_voice_count(a->store, ch);
      for (size_t i = 0; i < n; i++) {
        const ccord_voice_state *vs = ccord_store_voice_at(a->store, ch, i);
        if (!vs) continue;
        char idb[32];
        snprintf(idb, sizeof idb, "%llu", (unsigned long long)vs->user_id);
        ccord_jw_key(&w, idb);
        ccord_jw_double(&w, ccord_store_user_speaking(a->store, vs->user_id) ? 1.0 : 0.0);
      }
    }
  }
  ccord_jw_end_object(&w);
  char *out = (b.data && ccord_jw_ok(&w)) ? ccord_strdup((const char *)b.data) : nullptr;
  ccord_buf_free(&b);
  return out;
}

static bool plugin_op_voice_push_pcm(void *ud, const int16_t *pcm, size_t samples)
{
  (void)ud;
  (void)pcm;
  (void)samples;
  return false;
}

static char *plugin_op_media_current_json(void *ud)
{
  (void)ud;
  return ccord_strdup("{\"playing\":false,\"title\":\"\",\"artist\":\"\",\"album\":\"\"}");
}

static bool plugin_op_media_control(void *ud, const char *action)
{
  (void)ud;
  (void)action;
  return false;
}

static char *plugin_op_processes_list_json(void *ud)
{
  (void)ud;
  ccord_buf b = {0};
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_array(&w);
#if CCORD_OS_LINUX
  DIR *dir = opendir("/proc");
  if (dir) {
    struct dirent *de = nullptr;
    size_t count = 0;
    while ((de = readdir(dir)) != nullptr && count < 256u) {
      if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
      char *end = nullptr;
      long pid = strtol(de->d_name, &end, 10);
      if (!end || *end != '\0' || pid <= 0) continue;
      char pbuf[128], comm[128] = {0}, exe[512] = {0};
      snprintf(pbuf, sizeof pbuf, "/proc/%ld/comm", pid);
      FILE *fp = fopen(pbuf, "rb");
      if (!fp) continue;
      size_t n = fread(comm, 1, sizeof comm - 1u, fp);
      fclose(fp);
      while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r')) n--;
      comm[n] = '\0';
      if (!comm[0]) continue;
      snprintf(pbuf, sizeof pbuf, "/proc/%ld/exe", pid);
      ssize_t elen = readlink(pbuf, exe, sizeof exe - 1u);
      if (elen > 0) exe[elen] = '\0';
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "pid"); ccord_jw_int(&w, pid);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, comm);
      ccord_jw_key(&w, "exe"); ccord_jw_cstr(&w, exe);
      ccord_jw_end_object(&w);
      count++;
    }
    closedir(dir);
  }
#endif
  ccord_jw_end_array(&w);
  char *out = (b.data && ccord_jw_ok(&w)) ? ccord_strdup((const char *)b.data) : nullptr;
  ccord_buf_free(&b);
  return out;
}

static char *plugin_op_active_window_json(void *ud)
{
  app *a = ud;
  const char *title = "C-Cord";
  bool focused = false;
  if (a && a->platform) {
    focused = ccord_platform_window_focused(a->platform);
    SDL_Window *sw = ccord_platform_sdl_window(a->platform);
    if (sw) {
      const char *t = SDL_GetWindowTitle(sw);
      if (t && t[0]) title = t;
    }
  }
  ccord_buf b = {0};
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "title"); ccord_jw_cstr(&w, title);
  ccord_jw_key(&w, "focused"); ccord_jw_bool(&w, focused);
  ccord_jw_end_object(&w);
  char *out = (b.data && ccord_jw_ok(&w)) ? ccord_strdup((const char *)b.data) : nullptr;
  ccord_buf_free(&b);
  return out;
}

static void plugin_op_global_shortcuts_sync(void *ud)
{
  app_host_keybinds_changed(ud);
}

static void plugin_raw_gateway_cb(void *ud, const char *event_name, const char *payload_json)
{
  app *a = ud;
  if (a && a->plugins) ccord_plugins_dispatch_raw_gateway(a->plugins, event_name, payload_json);
}

static ccord_plugin_platform_state plugins_current_state(const app *a)
{
  ccord_plugin_platform_state st = {
    .safe_top = a->last_safe.top,
    .safe_right = a->last_safe.right,
    .safe_bottom = a->last_safe.bottom,
    .safe_left = a->last_safe.left,
    .viewport_w = a->viewport_w,
    .viewport_h = a->viewport_h,
    .viewport_scale = a->renderer ? ccord_render_get_zoom(a->renderer) : 1.0f,
    .keyboard_height = a->last_kb_inset,
    .keyboard_visible = a->last_kb_inset > 1.0f,
    .landscape = a->viewport_w > a->viewport_h,
  };
  return st;
}

void app_plugins_push_platform_state(app *a)
{
  if (!a || !a->plugins) return;
  ccord_plugin_platform_state st = plugins_current_state(a);
  ccord_plugins_update_platform_state(a->plugins, &st);
}

void app_plugins_start(app *a)
{
  if (a->plugins || !a->opt) return;
  char bundled[1024];
  char probe[1100];
  snprintf(bundled, sizeof bundled, "%s", CCORD_EXTENSIONS_DIR);   // bundled extensions shipped in the repo
  snprintf(probe, sizeof probe, "%s/copy-message-id/plugin.json", bundled);
  if (!bundled[0] || !ccord_path_is_file(probe)) {
    const char *ddir = ccord_paths_data_dir();
    if (ddir && ddir[0] && (size_t)snprintf(bundled, sizeof bundled, "%s/bundled_extensions", ddir) < sizeof bundled) {
      (void)ccord_embedded_materialize_extensions(bundled);
    }
  }
  const char *dirs[MAX_PLUGIN_DIRS + 2];
  size_t nd = 0;
  dirs[nd++] = bundled;
  char installed_dir[1024] = "";
  const char *ddir = ccord_paths_data_dir();
  if (ddir && ddir[0] && (size_t)snprintf(installed_dir, sizeof installed_dir, "%s/plugins/installed", ddir) < sizeof installed_dir) {
    dirs[nd++] = installed_dir;
  }
  for (size_t i = 0; i < a->opt->plugin_dir_count; i++) dirs[nd++] = a->opt->plugin_dirs[i];
  const char *host_exe = getenv("CCORD_PLUGIN_HOST");
  const char *chosen_host = (host_exe && *host_exe) ? host_exe : CCORD_PLUGIN_HOST_PATH;
  if ((!host_exe || !*host_exe) && (!chosen_host[0] || !ccord_path_is_file(chosen_host))) {
    chosen_host = ccord_paths_exe_path();
  }
  ccord_form_factor ff = a->platform ? ccord_platform_form_factor(a->platform) : CCORD_FORM_DESKTOP;
  bool is_mob = (a->opt && a->opt->mobile) || (a->platform && ccord_platform_is_mobile(a->platform));
  bool has_touch = (a->opt && a->opt->mobile) || (a->platform && ccord_platform_has_touch(a->platform));
  const char *ff_str = ff == CCORD_FORM_PHONE ? "phone" : (ff == CCORD_FORM_TABLET ? "tablet" : (is_mob ? "phone" : "desktop"));
  uint64_t caps = CCORD_CAP_NOTIFICATIONS | CCORD_CAP_SCREEN_CAPTURE | CCORD_CAP_PANELS;
  if (has_touch) caps |= CCORD_CAP_TOUCH;
  if (a->platform && ccord_platform_has_haptics(a->platform)) caps |= CCORD_CAP_HAPTICS;
  if (a->platform && ccord_platform_has_share(a->platform)) caps |= CCORD_CAP_SHARE;
#if CCORD_OS_ANDROID
  caps |= CCORD_CAP_SAFE_AREA | CCORD_CAP_KEYBOARD_CONTROL | CCORD_CAP_ORIENTATION_CONTROL | CCORD_CAP_KEEP_SCREEN_ON;
#else
  caps |= CCORD_CAP_TRAY | CCORD_CAP_GLOBAL_SHORTCUTS | CCORD_CAP_RPC | CCORD_CAP_HOVER;
  if (is_mob) caps |= CCORD_CAP_SAFE_AREA | CCORD_CAP_KEYBOARD_CONTROL;
#endif
  ccord_plugins_config pcfg = {
    .host_exe = chosen_host,
    .in_process = CCORD_OS_ANDROID ? true : false,
    .in_process_host_fn = ccord_plugin_host_run_fd,
    .search_dirs = dirs, .search_dir_count = nd, .data_dir = ccord_paths_data_dir(),
    .bus = a->bus, .store = a->store, .session = a->session, .contrib = a->contrib,
    .clipboard_write = plugin_clipboard, .clipboard_ud = a,
    .has_platform_info = true,
    .platform_info = {
      .os = ccord_os_name(),
      .arch = ccord_os_arch(),
      .form_factor = ff_str,
      .is_mobile = is_mob,
      .touch = has_touch,
      .android_api_level = ccord_os_android_api_level(),
      .capabilities = caps,
    },
    .platform_state = plugins_current_state(a),
    .platform_ops = {
      .ud = a,
      .haptic_impact = plugin_op_haptic_impact,
      .haptic_notify = plugin_op_haptic_notify,
      .share_text = plugin_op_share_text,
      .keyboard_dismiss = plugin_op_keyboard_dismiss,
      .keep_screen_on = plugin_op_keep_screen_on,
      .set_orientation = plugin_op_set_orientation,
      .open_url = plugin_op_open_url,
      .notify = plugin_op_notify,
      .clipboard_read = plugin_op_clipboard_read,
      .clipboard_read_image = plugin_op_clipboard_read_image,
      .clipboard_write_image = plugin_op_clipboard_write_image,
      .http_request = plugin_op_http_request,
      .ws_open = plugin_op_ws_open,
      .ws_send = plugin_op_ws_send,
      .ws_recv = plugin_op_ws_recv,
      .ws_close = plugin_op_ws_close,
      .theme_current_json = plugin_op_theme_current_json,
      .theme_color_hex = plugin_op_theme_color_hex,
      .theme_metric_val = plugin_op_theme_metric_val,
      .theme_list_json = plugin_op_theme_list_json,
      .scheduled_add = plugin_op_scheduled_add,
      .scheduled_list_json = plugin_op_scheduled_list_json,
      .scheduled_cancel = plugin_op_scheduled_cancel,
      .voice_levels_json = plugin_op_voice_levels_json,
      .voice_push_pcm = plugin_op_voice_push_pcm,
      .media_current_json = plugin_op_media_current_json,
      .media_control = plugin_op_media_control,
      .processes_list_json = plugin_op_processes_list_json,
      .active_window_json = plugin_op_active_window_json,
      .global_shortcuts_sync = plugin_op_global_shortcuts_sync,
    },
    .dev_auto_reload = a->opt->plugin_dev,
  };
  if (!a->virtual_clock) {
    pcfg.catalog_http = (ccord_catalog_http){
      .ud = a,
      .get = catalog_http_get_adapter,
      .post_json = catalog_http_post_adapter,
      .download_file = catalog_http_download_adapter,
    };
  }
  ccord_error err = {0};
  a->plugins = ccord_plugins_create(&pcfg, &err);
  if (!a->plugins) {
    CCORD_LOG_W("extensions disabled: %s", err.message);
    return;
  }
  a->plugin_raw_gw_active = false;
  (void)ccord_plugins_load_all(a->plugins);
  app_plugins_sync_raw_gateway(a);
}

void app_plugins_sync_raw_gateway(app *a)
{
  if (!a || !a->session) return;
  bool need = a->plugins && ccord_plugins_has_raw_gateway_subscribers(a->plugins);
  if (need == a->plugin_raw_gw_active) return;
  a->plugin_raw_gw_active = need;
  ccord_session_set_raw_gateway_cb(a->session, need ? plugin_raw_gateway_cb : nullptr, need ? a : nullptr);
}

// Stops every host (they send nothing more) and drops their menu items; Settings keeps the plugin list as Stopped.
void app_plugins_stop(app *a)
{
  if (!a->plugins) return;
  if (a->session) ccord_session_set_raw_gateway_cb(a->session, nullptr, nullptr);
  a->plugin_raw_gw_active = false;
  ccord_plugins_destroy(a->plugins);
  a->plugins = nullptr;
  ccord_contrib_set_handlers(a->contrib, &(ccord_contrib_handlers){0});   // no callbacks into the freed manager
}

void app_host_set_extensions(void *ud, bool on)
{
  app *a = ud;
  if (on) app_plugins_start(a);
  else app_plugins_stop(a);
  ccord_ui_set_extensions_enabled(a->ui, a->plugins != nullptr);   // report failure back to the switch
}
#endif

