// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
#include "plugins_internal.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/clock.h>
#include <ccord/core/input.h>
#include <ccord/core/log.h>
#include <ccord/core/utf8.h>
#include <ccord/store/expressions.h>
#include <ccord/store/roles.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern const ccord_json_limits k_ext_limits;

// Shared helpers (api_v3_system.c)
bool ext_is_localhost_url(const char *url);
uint32_t ext_parse_hex_rgba(const char *s);
bool ext_resolve_plugin_asset_path(const plugin *p, const char *rel, char *out, size_t cap);
void ext_encode_json_str(const ccord_json *j, ccord_buf *out);
void ext_send_async_reply(ccord_plugins *m, plugin *p, uint64_t req_id, bool ok,
                          const char *result_json, const char *err_msg);
bool ext_sync_host_call(ccord_plugins *m, plugin *p, const char *method, const char *params_json,
                        uint32_t timeout_ms, char **out_json, char *err_buf, size_t err_cap);

// Sub-dispatchers
bool ext_handle_settings_and_theme(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params);
bool ext_handle_ui_v3(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params);
bool ext_handle_audio_hooks_gw_rpc(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params);
bool ext_handle_messages_store_v3(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params);
bool ext_handle_presence_voice_social_window(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params);
bool ext_handle_account_v3(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params);
bool ext_handle_class3_v3(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params);
