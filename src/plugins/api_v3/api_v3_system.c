// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "api_v3_internal.h"

#include <ccord/core/base64.h>

const ccord_json_limits k_ext_limits = {16, 8192, (size_t)2u << 20};

// ---------------------------------------------------------------- helpers

bool ext_is_localhost_url(const char *url)
{
  if (!url) return false;
  char scheme[16] = "", host[256] = "", path[1024] = "";
  int port = 80;
  if (!pw_extract_url_host_port(url, scheme, sizeof scheme, host, sizeof host, &port, path, sizeof path)) {
    return false;
  }
  if (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0 &&
      strcmp(scheme, "ws") != 0 && strcmp(scheme, "wss") != 0) {
    return false;
  }
  return pw_is_localhost_host(host);
}

uint32_t ext_parse_hex_rgba(const char *s)
{
  if (!s || s[0] != '#') return 0;
  size_t n = strlen(s);
  if (n != 7 && n != 9) return 0;
  unsigned long v = strtoul(s + 1, nullptr, 16);
  if (n == 7) return ((uint32_t)v << 8) | 0xFFu;
  return (uint32_t)v;
}

bool ext_resolve_plugin_asset_path(const plugin *p, const char *rel, char *out, size_t cap)
{
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!rel || !rel[0]) return true;
  if (!p || !p->dir || !p->dir[0]) return false;
  if (rel[0] == '/' || strstr(rel, "..")) return false;
  char joined[PBUF], rdir[CCORD_PATH_MAX], rpath[CCORD_PATH_MAX];
  int n = snprintf(joined, sizeof joined, "%s/%s", p->dir, rel);
  if (n < 0 || (size_t)n >= sizeof joined) return false;
  if (!pl_resolve_real_path(p->dir, rdir, sizeof rdir)) return false;
  if (!pl_resolve_real_path(joined, rpath, sizeof rpath)) return false;
  if (!pl_path_strictly_under(rpath, rdir)) return false;
  if (strlen(rpath) >= cap) return false;
  snprintf(out, cap, "%s", rpath);
  return true;
}

void ext_encode_json_str(const ccord_json *j, ccord_buf *out)
{
  if (!j) return;
  ccord_jw w;
  ccord_jw_init(&w, out);
  pl_jw_copy_json(&w, j);
}

void ext_send_async_reply(ccord_plugins *m, plugin *p, uint64_t req_id, bool ok,
                             const char *result_json, const char *err_msg)
{
  if (!p || !p->ipc || (p->state != CCORD_PLUGIN_READY && p->state != CCORD_PLUGIN_STARTING)) return;
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "req_id"); ccord_jw_int(&w, (int64_t)req_id);
  ccord_jw_key(&w, "ok"); ccord_jw_bool(&w, ok);
  if (ok && result_json && result_json[0]) {
    ccord_arena ar; ccord_arena_init(&ar, 0); ccord_error pe = {0};
    const ccord_json *rj = ccord_json_parse(&ar, result_json, strlen(result_json), &k_ext_limits, &pe);
    ccord_jw_key(&w, "result");
    if (rj) pl_jw_copy_json(&w, rj);
    else ccord_jw_null(&w);
    ccord_arena_free(&ar);
  }
  if (!ok) {
    ccord_jw_key(&w, "error"); ccord_jw_cstr(&w, err_msg && err_msg[0] ? err_msg : "request failed");
  }
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "async.reply", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

// Bounded synchronous RPC from main -> plugin host. Interleaved host requests are served inline to avoid deadlocks.
bool ext_sync_host_call(ccord_plugins *m, plugin *p, const char *method, const char *params_json,
                           uint32_t timeout_ms, char **out_json, char *err_buf, size_t err_cap)
{
  if (out_json) *out_json = nullptr;
  if (err_buf && err_cap) err_buf[0] = '\0';
  if (!m || !p || !p->ipc || p->state != CCORD_PLUGIN_READY) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "plugin is not running");
    return false;
  }
  uint32_t id = pl_next_request_id(p);
  ccord_error err = {0};
  if (ccord_ipc_send_request(p->ipc, id, method, params_json ? params_json : "{}", &err) != CCORD_OK ||
      ccord_ipc_flush(p->ipc, (int)timeout_ms, &err) != CCORD_OK) {
    if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", err.message[0] ? err.message : "ipc send failed");
    return false;
  }
  uint64_t deadline = ccord_clock_ms() + timeout_ms;
  ccord_arena ar;
  ccord_arena_init(&ar, 4096u);
  bool done = false, ok = false;
  while (!done && p->ipc) {
    uint64_t now = ccord_clock_ms();
    if (now >= deadline) {
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "hook/rpc timeout");
      break;
    }
    int rem = (int)(deadline - now);
    if (rem > 50) rem = 50;
    ccord_arena_reset(&ar);
    ccord_ipc_msg msg;
    ccord_status wst = ccord_ipc_wait(p->ipc, &ar, &msg, rem, &err);
    if (wst == CCORD_ERR_TIMEOUT) continue;
    if (wst != CCORD_OK) {
      if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", err.message[0] ? err.message : "ipc closed");
      break;
    }
    if (msg.kind == CCORD_IPC_RES && msg.id == id) {
      done = true;
      ok = msg.ok;
      if (!ok) {
        if (err_buf && err_cap) snprintf(err_buf, err_cap, "%s", msg.err_message ? msg.err_message : "failed");
      } else if (out_json && msg.result) {
        ccord_buf b; ccord_buf_init(&b, 0);
        ext_encode_json_str(msg.result, &b);
        if (!b.failed) *out_json = ccord_strndup(ccord_buf_cstr(&b), b.len);
        ccord_buf_free(&b);
      }
      break;
    }
    // Serve interleaved host request/notification/response without losing it.
    pl_handle_frame(m, p, &msg);
    if (p->ipc) (void)ccord_ipc_flush(p->ipc, 0, &err);
  }
  ccord_arena_free(&ar);
  return ok;
}

static bool env_var_is_blocked(const char *name)
{
  if (!name || !name[0]) return true;
  char up[128];
  size_t n = strlen(name);
  if (n >= sizeof up) return true;
  for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)name[i]);
  up[n] = '\0';
  if (strstr(up, "DISCORD_TOKEN") || strstr(up, "CCORD_TOKEN") || strstr(up, "AUTH_TOKEN") ||
      strstr(up, "CCORD_SECRET") || strstr(up, "SESSION_TOKEN")) {
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- Worker event pump

static bool ext_send_ntf_flushed(ccord_plugins *m, plugin *p, const char *method, const char *params)
{
  if (!p || !p->ipc) return false;
  ccord_error err = {0};
  if (ccord_ipc_flush(p->ipc, 500, &err) != CCORD_OK) {
    char why[256];
    snprintf(why, sizeof why, "could not flush host ipc: %s", err.message);
    pl_fail_plugin(m, p, why);
    return false;
  }
  if (ccord_ipc_send_notify(p->ipc, method, params, &err) != CCORD_OK) {
    char why[256];
    snprintf(why, sizeof why, "could not send to host: %s", err.message);
    pl_fail_plugin(m, p, why);
    return false;
  }
  if (ccord_ipc_flush(p->ipc, 500, &err) != CCORD_OK) {
    char why[256];
    snprintf(why, sizeof why, "could not flush host ipc: %s", err.message);
    pl_fail_plugin(m, p, why);
    return false;
  }
  return true;
}

static void ext_send_http_reply(ccord_plugins *m, plugin *p, const pw_event *ev)
{
  if (!p || !p->ipc || (p->state != CCORD_PLUGIN_READY && p->state != CCORD_PLUGIN_STARTING)) return;
  if (!ev->ok) {
    ext_send_async_reply(m, p, ev->id, false, nullptr, ev->error);
    return;
  }
  if (!ev->http_binary && ev->raw_body_len > 0 &&
      !ccord_utf8_valid((const char *)ev->raw_body, ev->raw_body_len)) {
    ext_send_async_reply(m, p, ev->id, false, nullptr,
                         "response body is not valid UTF-8 (pass binary = true)");
    return;
  }

  const size_t inline_limit = 512u * 1024u;
  const size_t chunk_raw = 384u * 1024u;
  size_t hdr_len = ev->json ? strlen(ev->json) : 0u;
  bool use_chunked = (ev->raw_body_len > inline_limit) ||
                     (ev->http_binary && ev->raw_body_len > 0 && hdr_len > 128u * 1024u);

  if (use_chunked) {
    size_t off = 0;
    uint64_t seq = 0;
    while (off < ev->raw_body_len) {
      size_t n = ev->raw_body_len - off;
      if (n > chunk_raw) n = chunk_raw;
      size_t b64_len = 0;
      char *b64 = ccord_base64_encode_alloc(ev->raw_body + off, n, &b64_len);
      if (!b64) {
        ext_send_async_reply(m, p, ev->id, false, nullptr, "out of memory encoding chunk");
        return;
      }
      ccord_buf cb; ccord_buf_init(&cb, b64_len + 96u);
      ccord_jw cw; ccord_jw_init(&cw, &cb);
      ccord_jw_begin_object(&cw);
      ccord_jw_key(&cw, "req_id"); ccord_jw_int(&cw, (int64_t)ev->id);
      ccord_jw_key(&cw, "seq"); ccord_jw_int(&cw, (int64_t)seq);
      ccord_jw_key(&cw, "b64"); ccord_jw_string(&cw, b64, b64_len);
      ccord_jw_end_object(&cw);
      ccord_free(b64);
      bool ok = ccord_jw_ok(&cw) && ext_send_ntf_flushed(m, p, "async.reply_chunk", ccord_buf_cstr(&cb));
      ccord_buf_free(&cb);
      if (!ok) return;
      off += n;
      seq++;
    }
    ccord_buf b; ccord_buf_init(&b, hdr_len + 256u);
    ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "req_id"); ccord_jw_int(&w, (int64_t)ev->id);
    ccord_jw_key(&w, "ok"); ccord_jw_bool(&w, true);
    ccord_jw_key(&w, "result");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "status"); ccord_jw_int(&w, (int64_t)ev->http_status);
    ccord_jw_key(&w, "ok"); ccord_jw_bool(&w, ev->http_status >= 200 && ev->http_status < 300);
    ccord_jw_key(&w, "chunked"); ccord_jw_bool(&w, true);
    if (ev->json && ev->json[0]) {
      ccord_jw_key(&w, "headers_json"); ccord_jw_cstr(&w, ev->json);
    }
    if (ev->final_url && ev->final_url[0]) {
      ccord_jw_key(&w, "final_url"); ccord_jw_cstr(&w, ev->final_url);
    }
    ccord_jw_end_object(&w);
    ccord_jw_end_object(&w);
    if (ccord_jw_ok(&w)) (void)ext_send_ntf_flushed(m, p, "async.reply", ccord_buf_cstr(&b));
    ccord_buf_free(&b);
    return;
  }

  // Inline single-frame reply (<= 512 KiB)
  ccord_buf b; ccord_buf_init(&b, ev->raw_body_len + hdr_len + 256u);
  ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "req_id"); ccord_jw_int(&w, (int64_t)ev->id);
  ccord_jw_key(&w, "ok"); ccord_jw_bool(&w, true);
  ccord_jw_key(&w, "result");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "status"); ccord_jw_int(&w, (int64_t)ev->http_status);
  ccord_jw_key(&w, "ok"); ccord_jw_bool(&w, ev->http_status >= 200 && ev->http_status < 300);
  if (ev->http_binary) {
    ccord_jw_key(&w, "binary"); ccord_jw_bool(&w, true);
    if (ev->raw_body_len > 0) {
      size_t b64_len = 0;
      char *b64 = ccord_base64_encode_alloc(ev->raw_body, ev->raw_body_len, &b64_len);
      if (!b64) {
        ccord_buf_free(&b);
        ext_send_async_reply(m, p, ev->id, false, nullptr, "out of memory encoding body");
        return;
      }
      ccord_jw_key(&w, "body_b64"); ccord_jw_string(&w, b64, b64_len);
      ccord_free(b64);
    } else {
      ccord_jw_key(&w, "body_b64"); ccord_jw_cstr(&w, "");
    }
  } else {
    ccord_jw_key(&w, "body");
    ccord_jw_string(&w, ev->raw_body ? (const char *)ev->raw_body : "", ev->raw_body_len);
  }
  if (ev->json && ev->json[0]) {
    ccord_jw_key(&w, "headers_json"); ccord_jw_cstr(&w, ev->json);
  }
  if (ev->final_url && ev->final_url[0]) {
    ccord_jw_key(&w, "final_url"); ccord_jw_cstr(&w, ev->final_url);
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) (void)ext_send_ntf_flushed(m, p, "async.reply", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

void plugins_ext_on_worker_event(void *ud, const pw_event *ev)
{
  ccord_plugins *m = ud;
  if (!m || !ev) return;
  plugin *p = pl_find_owner(m, ev->owner);
  if (!p || !p->ipc || (p->state != CCORD_PLUGIN_READY && p->state != CCORD_PLUGIN_STARTING)) return;

  if (ev->kind == PW_EV_ASYNC_REPLY) {
    ext_send_async_reply(m, p, ev->id, ev->ok, ev->json, ev->error);
    return;
  }
  if (ev->kind == PW_EV_HTTP_REPLY) {
    ext_send_http_reply(m, p, ev);
    return;
  }
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_arena ar; ccord_arena_init(&ar, 0);
  const ccord_json *dj = nullptr;
  if (ev->json && ev->json[0]) {
    ccord_error pe = {0};
    dj = ccord_json_parse(&ar, ev->json, strlen(ev->json), &k_ext_limits, &pe);
  }
  ccord_jw_begin_object(&w);
  if (ev->kind == PW_EV_FS_WATCH) {
    ccord_jw_key(&w, "watch_id"); ccord_jw_int(&w, (int64_t)ev->id);
    ccord_jw_key(&w, "event");
    if (dj) pl_jw_copy_json(&w, dj); else ccord_jw_null(&w);
    ccord_jw_end_object(&w);
    if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "fs.watch.event", ccord_buf_cstr(&b));
  } else if (ev->kind == PW_EV_WS) {
    ccord_jw_key(&w, "ws_id"); ccord_jw_int(&w, (int64_t)ev->id);
    ccord_jw_key(&w, "kind"); ccord_jw_cstr(&w, ev->sub_kind);
    ccord_jw_key(&w, "data");
    if (dj) pl_jw_copy_json(&w, dj); else ccord_jw_null(&w);
    ccord_jw_end_object(&w);
    if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "net.ws.event", ccord_buf_cstr(&b));
  } else if (ev->kind == PW_EV_PROC) {
    ccord_jw_key(&w, "proc_id"); ccord_jw_int(&w, (int64_t)ev->id);
    ccord_jw_key(&w, "kind"); ccord_jw_cstr(&w, ev->sub_kind);
    ccord_jw_key(&w, "data");
    if (dj) pl_jw_copy_json(&w, dj); else ccord_jw_null(&w);
    ccord_jw_end_object(&w);
    if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "system.spawn.event", ccord_buf_cstr(&b));
  }
  ccord_arena_free(&ar);
  ccord_buf_free(&b);
}

// ---------------------------------------------------------------- --plugin-dev auto-reloader

static int64_t scan_plugin_mtime(const char *dir)
{
  if (!dir || !dir[0]) return 0;
  int64_t best = 0;
  ccord_dir *d = ccord_dir_open(dir);
  if (!d) return 0;
  char name[256];
  while (ccord_dir_next(d, name, sizeof name, nullptr)) {
    if (name[0] == '.') continue;
    char path[PBUF + 64];
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (n <= 0 || (size_t)n >= sizeof path) continue;
    struct stat st;
    if (stat(path, &st) == 0) {
      int64_t mt = (int64_t)st.st_mtime;
      if (mt > best) best = mt;
    }
  }
  ccord_dir_close(d);
  return best;
}

void plugins_ext_dev_poll(ccord_plugins *m, uint64_t now_ms)
{
  if (!m || !m->dev_auto_reload) return;
  if (now_ms - m->last_dev_scan_ms < 750u) return;
  m->last_dev_scan_ms = now_ms;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    if (!p->used || !p->dir || !p->dir[0] || !p->enabled) continue;
    if (p->state != CCORD_PLUGIN_READY && p->state != CCORD_PLUGIN_FAILED) continue;
    int64_t mt = scan_plugin_mtime(p->dir);
    if (mt == 0) continue;
    if (p->dev_mtime_sec == 0) {
      p->dev_mtime_sec = mt;
    } else if (mt > p->dev_mtime_sec) {
      p->dev_mtime_sec = mt;
      CCORD_LOG_I("plugin-dev: reloading %s (source changed)", p->m.id);
      pl_reload_plugin(m, p->m.id);
    }
  }
}

bool ext_handle_class3_v3(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params)
{
  if (strcmp(method, "net.http.request") == 0) {
    int64_t req_id = 0; (void)ccord_json_int64(ccord_json_get(params, "req_id"), &req_id);
    bool local_only = ccord_json_bool(ccord_json_get(params, "local_only"), false);
    const ccord_json *opts = ccord_json_get(params, "opts");
    size_t ul = 0, ml = 0, bl = 0;
    const char *url = pl_str_param(params, "url", &ul);
    if (!url && opts) url = pl_str_param(opts, "url", &ul);
    const char *hmeth = pl_str_param(params, "method", &ml);
    if (!hmeth && opts) hmeth = pl_str_param(opts, "method", &ml);
    const char *body = pl_str_param(params, "body", &bl);
    if (!body && opts) body = pl_str_param(opts, "body", &bl);
    int64_t tmo = 15000;
    if (opts && ccord_json_get(opts, "timeout_ms")) (void)ccord_json_int64(ccord_json_get(opts, "timeout_ms"), &tmo);
    int64_t max_b = 0;
    if (opts && ccord_json_get(opts, "max_bytes")) (void)ccord_json_int64(ccord_json_get(opts, "max_bytes"), &max_b);
    bool binary = opts ? ccord_json_bool(ccord_json_get(opts, "binary"), false) : false;
    bool follow_redir = opts ? ccord_json_bool(ccord_json_get(opts, "follow_redirects"), false) : false;

    bool is_loc = ext_is_localhost_url(url);
    bool allow_local = pl_has_perm(p, CCORD_PERM_NETWORK_LOCAL);
    if (local_only) {
      if (!allow_local) { pl_fail_plugin(m, p, "undeclared restricted API called: network.local"); return true; }
    } else if (is_loc) {
      if (!allow_local) {
        if (!pl_has_perm(p, CCORD_PERM_NETWORK_HTTP)) {
          pl_fail_plugin(m, p, "undeclared restricted API called: network.local");
          return true;
        }
        pl_reply_ok(m, p, id, nullptr);
        ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr,
                             "localhost access requires network.local permission");
        return true;
      }
    } else if (!pl_has_perm(p, CCORD_PERM_NETWORK_HTTP)) {
      pl_fail_plugin(m, p, "undeclared restricted API called: network.http");
      return true;
    }
    ccord_buf hb; ccord_buf_init(&hb, 0);
    if (opts && ccord_json_get(opts, "headers")) ext_encode_json_str(ccord_json_get(opts, "headers"), &hb);
    pl_reply_ok(m, p, id, nullptr);
    if (!m->workers || !url ||
        !pw_start_http(m->workers, p->m.id, (uint64_t)req_id, &p->m, url, hmeth ? hmeth : "GET",
                       hb.len ? ccord_buf_cstr(&hb) : nullptr, (const uint8_t *)body, bl,
                       (uint32_t)(tmo > 0 ? tmo : 15000), max_b > 0 ? (size_t)max_b : 0u,
                       binary, follow_redir, local_only, allow_local)) {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, "http worker unavailable");
    }
    ccord_buf_free(&hb);
    return true;
  }
  if (strcmp(method, "net.http.download") == 0) {
    int64_t req_id = 0; (void)ccord_json_int64(ccord_json_get(params, "req_id"), &req_id);
    const ccord_json *opts = ccord_json_get(params, "opts");
    size_t ul = 0, dl = 0;
    const char *url = pl_str_param(params, "url", &ul);
    if (!url && opts) url = pl_str_param(opts, "url", &ul);
    const char *dest_path = pl_str_param(params, "dest_path", &dl);
    if (!dest_path && opts) dest_path = pl_str_param(opts, "dest_path", &dl);
    int64_t tmo = 30000;
    if (opts && ccord_json_get(opts, "timeout_ms")) (void)ccord_json_int64(ccord_json_get(opts, "timeout_ms"), &tmo);
    int64_t max_b = 0;
    if (opts && ccord_json_get(opts, "max_bytes")) (void)ccord_json_int64(ccord_json_get(opts, "max_bytes"), &max_b);
    bool follow_redir = opts ? ccord_json_bool(ccord_json_get(opts, "follow_redirects"), false) : false;

    bool is_loc = ext_is_localhost_url(url);
    bool allow_local = pl_has_perm(p, CCORD_PERM_NETWORK_LOCAL);
    if (is_loc) {
      if (!allow_local) {
        if (!pl_has_perm(p, CCORD_PERM_NETWORK_HTTP)) {
          pl_fail_plugin(m, p, "undeclared restricted API called: network.local");
          return true;
        }
        pl_reply_ok(m, p, id, nullptr);
        ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr,
                             "localhost access requires network.local permission");
        return true;
      }
    } else if (!pl_has_perm(p, CCORD_PERM_NETWORK_HTTP)) {
      pl_fail_plugin(m, p, "undeclared restricted API called: network.http");
      return true;
    }

    if (!dest_path || dl == 0 || dl >= CCORD_PATH_MAX || !ccord_utf8_valid(dest_path, dl)) {
      pl_reply_ok(m, p, id, nullptr);
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, "invalid destination path");
      return true;
    }
    pl_path_scope scope = PL_PATH_HOST_FS;
    char resolved_dest[CCORD_PATH_MAX];
    const char *rcode = "invalid_argument";
    char rerr[256] = "";
    if (!pl_resolve_resource_path(m, p, dest_path, true, &scope, resolved_dest, sizeof resolved_dest,
                                  &rcode, rerr, sizeof rerr)) {
      if (p->state == CCORD_PLUGIN_READY || p->state == CCORD_PLUGIN_STARTING) {
        pl_reply_ok(m, p, id, nullptr);
        ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr,
                             rerr[0] ? rerr : "permission denied: destination path rejected");
      }
      return true;
    }

    ccord_buf hb; ccord_buf_init(&hb, 0);
    if (opts && ccord_json_get(opts, "headers")) ext_encode_json_str(ccord_json_get(opts, "headers"), &hb);
    pl_reply_ok(m, p, id, nullptr);
    if (!m->workers || !url ||
        !pw_start_http_download(m->workers, p->m.id, (uint64_t)req_id, &p->m, url, resolved_dest,
                                hb.len ? ccord_buf_cstr(&hb) : nullptr,
                                (uint32_t)(tmo > 0 ? tmo : 30000), max_b > 0 ? (size_t)max_b : 0u,
                                follow_redir, false, allow_local)) {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, "http download worker unavailable");
    }
    ccord_buf_free(&hb);
    return true;
  }
  if (strcmp(method, "net.local.unix_send") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_NETWORK_LOCAL)) { pl_fail_plugin(m, p, "undeclared restricted API called: network.local"); return true; }
    int64_t req_id = 0; (void)ccord_json_int64(ccord_json_get(params, "req_id"), &req_id);
    size_t pl_l = 0, bl = 0;
    const char *spath = pl_str_param(params, "path", &pl_l);
    const char *b64 = pl_str_param(params, "b64", &bl);
    if (!spath || !spath[0] || pl_fs_path_is_protected(m, spath) || pw_unix_socket_path_is_blocked(spath)) {
      pl_reply_ok(m, p, id, nullptr);
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr,
                           "permission denied: unix socket path is protected");
      return true;
    }
    size_t cap = (bl / 4u) * 3u + 4u;
    uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
    size_t rlen = 0;
    bool dec_ok = !bl || (raw && pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen));
    pl_reply_ok(m, p, id, nullptr);
    if (!m->workers || !dec_ok || !pw_start_unix_send(m->workers, p->m.id, (uint64_t)req_id, spath, raw, rlen)) {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, "unix socket send failed");
    }
    ccord_free(raw);
    return true;
  }
  if (strcmp(method, "net.ws.connect") == 0) {
    int64_t wid = 0; (void)ccord_json_int64(ccord_json_get(params, "ws_id"), &wid);
    size_t ul = 0; const char *url = pl_str_param(params, "url", &ul);
    bool is_loc = ext_is_localhost_url(url);
    bool allow_local = pl_has_perm(p, CCORD_PERM_NETWORK_LOCAL);
    if (is_loc) {
      if (!allow_local) {
        if (!pl_has_perm(p, CCORD_PERM_NETWORK_WS)) {
          pl_fail_plugin(m, p, "undeclared restricted API called: network.local");
          return true;
        }
        if (m->workers && url) {
          (void)pw_ws_connect(m->workers, p->m.id, (uint64_t)wid, &p->m, url, nullptr, false);
        }
        pl_reply_ok(m, p, id, nullptr);
        return true;
      }
    } else if (!pl_has_perm(p, CCORD_PERM_NETWORK_WS)) {
      pl_fail_plugin(m, p, "undeclared restricted API called: network.ws");
      return true;
    }
    ccord_buf hb; ccord_buf_init(&hb, 0);
    if (ccord_json_get(params, "headers")) ext_encode_json_str(ccord_json_get(params, "headers"), &hb);
    if (m->workers && url) {
      (void)pw_ws_connect(m->workers, p->m.id, (uint64_t)wid, &p->m, url, hb.len ? ccord_buf_cstr(&hb) : nullptr, allow_local);
    }
    ccord_buf_free(&hb);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "net.ws.send") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_NETWORK_WS) && !pl_has_perm(p, CCORD_PERM_NETWORK_LOCAL)) {
      pl_fail_plugin(m, p, "undeclared restricted API called: network.ws");
      return true;
    }
    int64_t wid = 0; (void)ccord_json_int64(ccord_json_get(params, "ws_id"), &wid);
    bool bin = ccord_json_bool(ccord_json_get(params, "binary"), false);
    if (bin) {
      size_t bl = 0; const char *b64 = pl_str_param(params, "b64", &bl);
      size_t cap = (bl / 4u) * 3u + 4u;
      uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
      size_t rlen = 0;
      if (m->workers && raw && pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen)) {
        (void)pw_ws_send(m->workers, p->m.id, (uint64_t)wid, raw, rlen, true);
      }
      ccord_free(raw);
    } else {
      size_t tl = 0; const char *txt = pl_str_param(params, "text", &tl);
      if (m->workers && txt) (void)pw_ws_send(m->workers, p->m.id, (uint64_t)wid, (const uint8_t *)txt, tl, false);
    }
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "net.ws.close") == 0) {
    int64_t wid = 0, code = 1000;
    (void)ccord_json_int64(ccord_json_get(params, "ws_id"), &wid);
    (void)ccord_json_int64(ccord_json_get(params, "code"), &code);
    size_t rl = 0; const char *rsn = pl_str_param(params, "reason", &rl);
    if (m->workers) pw_ws_close(m->workers, p->m.id, (uint64_t)wid, (uint16_t)code, rsn ? rsn : "");
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "fs.watch") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_FS_WATCH)) { pl_fail_plugin(m, p, "undeclared restricted API called: fs.watch"); return true; }
    int64_t wid = 0; (void)ccord_json_int64(ccord_json_get(params, "watch_id"), &wid);
    size_t pl_l = 0; const char *path = pl_str_param(params, "path", &pl_l);
    if (!path || pl_fs_path_is_protected(m, path)) {
      pl_reply_err(m, p, id, "permission_denied", "path is protected");
      return true;
    }
    if (!m->workers || !pw_fs_watch_add(m->workers, p->m.id, (uint64_t)wid, path)) {
      pl_reply_err(m, p, id, "io", "fs.watch failed");
    } else pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "fs.unwatch") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_FS_WATCH)) { pl_fail_plugin(m, p, "undeclared restricted API called: fs.watch"); return true; }
    int64_t wid = 0; (void)ccord_json_int64(ccord_json_get(params, "watch_id"), &wid);
    if (m->workers) pw_fs_watch_remove(m->workers, p->m.id, (uint64_t)wid);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "fs.stat") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_FS_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: fs.read"); return true; }
    size_t pl_l = 0; const char *path = pl_str_param(params, "path", &pl_l);
    if (!path || pl_fs_path_is_protected(m, path)) {
      pl_reply_err(m, p, id, "permission_denied", "path is protected");
      return true;
    }
    struct stat st;
#if !CCORD_OS_WINDOWS
    if (lstat(path, &st) != 0) { pl_reply_err(m, p, id, "io", strerror(errno)); return true; }
    if (S_ISLNK(st.st_mode)) { pl_reply_err(m, p, id, "permission_denied", "symbolic links are not allowed"); return true; }
#else
    if (stat(path, &st) != 0) { pl_reply_err(m, p, id, "io", strerror(errno)); return true; }
#endif
    char buf[160];
    snprintf(buf, sizeof buf, "{\"size\":%lld,\"mtime\":%lld,\"is_dir\":%s,\"is_file\":%s}",
             (long long)st.st_size, (long long)st.st_mtime,
             S_ISDIR(st.st_mode) ? "true" : "false", S_ISREG(st.st_mode) ? "true" : "false");
    pl_reply_ok(m, p, id, buf);
    return true;
  }
  if (strcmp(method, "system.exec") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_EXEC)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.exec"); return true; }
    int64_t req_id = 0; (void)ccord_json_int64(ccord_json_get(params, "req_id"), &req_id);
    size_t cl = 0, cwdl = 0, inl = 0;
    const char *cmd = pl_str_param(params, "cmd", &cl);
    const char *cwd = pl_str_param(params, "cwd", &cwdl);
    const char *stdin_s = pl_str_param(params, "stdin", &inl);
    int64_t tmo = 10000; (void)ccord_json_int64(ccord_json_get(params, "timeout_ms"), &tmo);
    const char *args[64]; size_t nargs = 0;
    const ccord_json *aj = ccord_json_get(params, "args");
    if (aj && ccord_json_type_of(aj) == CCORD_JSON_ARRAY) {
      for (size_t i = 0; i < ccord_json_array_len(aj) && nargs < 64u; i++) {
        size_t al = 0; const char *s = ccord_json_string(ccord_json_array_at(aj, i), &al);
        if (s) args[nargs++] = s;
      }
    }
    pl_reply_ok(m, p, id, nullptr);
    if (!m->workers || !cmd || !cmd[0] ||
        !pw_start_exec(m->workers, p->m.id, (uint64_t)req_id, cmd, args, nargs, cwd,
                       (const uint8_t *)stdin_s, inl, (uint32_t)(tmo > 0 ? tmo : 10000))) {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, "system.exec failed to start");
    }
    return true;
  }
  if (strcmp(method, "system.spawn") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_EXEC)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.exec"); return true; }
    int64_t pid = 0; (void)ccord_json_int64(ccord_json_get(params, "proc_id"), &pid);
    size_t cl = 0, cwdl = 0;
    const char *cmd = pl_str_param(params, "cmd", &cl);
    const char *cwd = pl_str_param(params, "cwd", &cwdl);
    const char *args[64]; size_t nargs = 0;
    const ccord_json *aj = ccord_json_get(params, "args");
    if (aj && ccord_json_type_of(aj) == CCORD_JSON_ARRAY) {
      for (size_t i = 0; i < ccord_json_array_len(aj) && nargs < 64u; i++) {
        size_t al = 0; const char *s = ccord_json_string(ccord_json_array_at(aj, i), &al);
        if (s) args[nargs++] = s;
      }
    }
    if (m->workers && cmd && cmd[0]) (void)pw_start_spawn(m->workers, p->m.id, (uint64_t)pid, cmd, args, nargs, cwd);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "system.spawn.write") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_EXEC)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.exec"); return true; }
    int64_t pid = 0; (void)ccord_json_int64(ccord_json_get(params, "proc_id"), &pid);
    size_t bl = 0; const char *b64 = pl_str_param(params, "b64", &bl);
    size_t cap = (bl / 4u) * 3u + 4u;
    uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
    size_t rlen = 0;
    if (m->workers && raw && pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen)) {
      (void)pw_proc_write(m->workers, p->m.id, (uint64_t)pid, raw, rlen);
    }
    ccord_free(raw);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "system.spawn.kill") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_EXEC)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.exec"); return true; }
    int64_t pid = 0; (void)ccord_json_int64(ccord_json_get(params, "proc_id"), &pid);
    if (m->workers) pw_proc_kill(m->workers, p->m.id, (uint64_t)pid);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "system.env") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_ENV)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.env"); return true; }
    size_t nl = 0; const char *name = pl_str_param(params, "name", &nl);
    if (!name || env_var_is_blocked(name)) { pl_reply_ok(m, p, id, "null"); return true; }
    const char *val = getenv(name);
    if (!val) { pl_reply_ok(m, p, id, "null"); return true; }
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_cstr(&w, val);
    pl_reply_ok(m, p, id, ccord_jw_ok(&w) ? ccord_buf_cstr(&b) : "null");
    ccord_buf_free(&b);
    return true;
  }
  if (strcmp(method, "system.paths") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_ENV)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.env"); return true; }
    const char *home = getenv("HOME");
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    char dl[CCORD_PATH_MAX] = "", desk[CCORD_PATH_MAX] = "";
    if (home && home[0]) {
      snprintf(dl, sizeof dl, "%s/Downloads", home);
      snprintf(desk, sizeof desk, "%s/Desktop", home);
    }
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "home"); ccord_jw_cstr(&w, home ? home : "");
    ccord_jw_key(&w, "temp"); ccord_jw_cstr(&w, tmp);
    ccord_jw_key(&w, "downloads"); ccord_jw_cstr(&w, dl);
    ccord_jw_key(&w, "desktop"); ccord_jw_cstr(&w, desk);
    ccord_jw_end_object(&w);
    pl_reply_ok(m, p, id, ccord_jw_ok(&w) ? ccord_buf_cstr(&b) : "{}");
    ccord_buf_free(&b);
    return true;
  }
  if (strcmp(method, "clipboard.read_image") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_CLIPBOARD_IMAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: clipboard.image"); return true; }
    uint8_t *png = nullptr; size_t len = 0;
    if (!m->plat_ops.clipboard_read_image || !m->plat_ops.clipboard_read_image(m->plat_ops.ud, &png, &len) || !png || len == 0) {
      pl_reply_ok(m, p, id, "{\"b64\":\"\"}");
      ccord_free(png);
      return true;
    }
    ccord_buf b64; ccord_buf_init(&b64, (len * 4u) / 3u + 8u);
    pl_fs_b64_encode(png, len, &b64);
    ccord_free(png);
    ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
    ccord_jw_begin_object(&w); ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len); ccord_jw_end_object(&w);
    ccord_buf_free(&b64);
    pl_reply_ok(m, p, id, ccord_jw_ok(&w) ? ccord_buf_cstr(&out) : "{\"b64\":\"\"}");
    ccord_buf_free(&out);
    return true;
  }
  if (strcmp(method, "clipboard.write_image") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_CLIPBOARD_IMAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: clipboard.image"); return true; }
    size_t bl = 0; const char *b64 = pl_str_param(params, "b64", &bl);
    size_t cap = (bl / 4u) * 3u + 4u;
    uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
    size_t rlen = 0;
    if (m->plat_ops.clipboard_write_image && raw && pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen)) {
      (void)m->plat_ops.clipboard_write_image(m->plat_ops.ud, raw, rlen);
    }
    ccord_free(raw);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "system.media.current") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_MEDIA)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.media"); return true; }
    char *j = m->plat_ops.media_current_json ? m->plat_ops.media_current_json(m->plat_ops.ud) : nullptr;
    pl_reply_ok(m, p, id, j ? j : "null");
    ccord_free(j);
    return true;
  }
  if (strcmp(method, "system.media.control") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_MEDIA)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.media"); return true; }
    size_t al = 0; const char *act = pl_str_param(params, "action", &al);
    if (act && m->plat_ops.media_control) (void)m->plat_ops.media_control(m->plat_ops.ud, act);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "system.processes.list") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_PROCESSES)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.processes"); return true; }
    char *j = m->plat_ops.processes_list_json ? m->plat_ops.processes_list_json(m->plat_ops.ud) : nullptr;
    pl_reply_ok(m, p, id, j ? j : "[]");
    ccord_free(j);
    return true;
  }
  if (strcmp(method, "system.processes.active_window") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_PROCESSES)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.processes"); return true; }
    char *j = m->plat_ops.active_window_json ? m->plat_ops.active_window_json(m->plat_ops.ud) : nullptr;
    pl_reply_ok(m, p, id, j ? j : "null");
    ccord_free(j);
    return true;
  }
  return false;
}

bool plugins_ext_handle_request(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  uint32_t id = msg->id;
  const char *method = msg->method;
  const ccord_json *params = msg->params;
  if (ext_handle_settings_and_theme(m, p, id, method, params)) return true;
  if (ext_handle_ui_v3(m, p, id, method, params)) return true;
  if (ext_handle_audio_hooks_gw_rpc(m, p, id, method, params)) return true;
  if (ext_handle_messages_store_v3(m, p, id, method, params)) return true;
  if (ext_handle_presence_voice_social_window(m, p, id, method, params)) return true;
  if (ext_handle_account_v3(m, p, id, method, params)) return true;
  if (ext_handle_class3_v3(m, p, id, method, params)) return true;
  return false;
}

