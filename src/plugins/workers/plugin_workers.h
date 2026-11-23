// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include <ccord/plugins/plugins.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct plugin_workers plugin_workers;

typedef enum pw_event_kind : uint8_t {
  PW_EV_ASYNC_REPLY = 0,
  PW_EV_FS_WATCH,
  PW_EV_WS,
  PW_EV_PROC,
} pw_event_kind;

typedef struct pw_event {
  pw_event_kind kind;
  char owner[65];
  uint64_t id;          // req_id / watch_id / ws_id / proc_id
  bool ok;
  char sub_kind[24];    // "open"|"message"|"close"|"error" or "stdout"|"stderr"|"exit"
  char *json;           // owned JSON payload (ccord_free)
  char error[256];
} pw_event;

plugin_workers *pw_create(const ccord_plugin_platform_ops *ops);
void pw_destroy(plugin_workers *w);
void pw_remove_owner(plugin_workers *w, const char *owner);
// Drains completed worker events into caller callback on main thread.
void pw_pump(plugin_workers *w, void (*on_event)(void *ud, const pw_event *ev), void *ud);

// URL & local target validation helpers
bool pw_extract_url_host_port(const char *url, char *scheme_out, size_t scap, char *host_out, size_t hcap,
                              int *port_out, char *path_out, size_t pcap);
bool pw_is_localhost_host(const char *host);
bool pw_unix_socket_path_is_blocked(const char *sock_path);

// Async HTTP / localhost HTTP
bool pw_start_http(plugin_workers *w, const char *owner, uint64_t req_id, const ccord_manifest *manifest,
                   const char *url, const char *method, const char *headers_json,
                   const uint8_t *body, size_t body_len, uint32_t timeout_ms, bool local_only, bool allow_local);
// Async local Unix socket send/recv
bool pw_start_unix_send(plugin_workers *w, const char *owner, uint64_t req_id, const char *sock_path,
                        const uint8_t *data, size_t len);
// WebSockets (loopback ws:// native RFC 6455; external wss:// via platform_ops.ws_*)
bool pw_ws_connect(plugin_workers *w, const char *owner, uint64_t ws_id, const ccord_manifest *manifest,
                   const char *url, const char *headers_json, bool allow_local);
bool pw_ws_send(plugin_workers *w, const char *owner, uint64_t ws_id, const uint8_t *data, size_t len, bool binary);
void pw_ws_close(plugin_workers *w, const char *owner, uint64_t ws_id, uint16_t code, const char *reason);
// Filesystem watcher
bool pw_fs_watch_add(plugin_workers *w, const char *owner, uint64_t watch_id, const char *path);
void pw_fs_watch_remove(plugin_workers *w, const char *owner, uint64_t watch_id);
// External process exec / streaming spawn (argv array, no shell)
bool pw_start_exec(plugin_workers *w, const char *owner, uint64_t req_id, const char *cmd,
                   const char *const *args, size_t nargs, const char *cwd, const uint8_t *stdin_data,
                   size_t stdin_len, uint32_t timeout_ms);
bool pw_start_spawn(plugin_workers *w, const char *owner, uint64_t proc_id, const char *cmd,
                    const char *const *args, size_t nargs, const char *cwd);
bool pw_proc_write(plugin_workers *w, const char *owner, uint64_t proc_id, const uint8_t *data, size_t len);
void pw_proc_kill(plugin_workers *w, const char *owner, uint64_t proc_id);
