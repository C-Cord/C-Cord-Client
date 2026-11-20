// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "plugins_internal.h"
#include "package/zip.h"

#include <ccord/core/alloc.h>
#include <ccord/core/arena.h>
#include <ccord/core/atomic_file.h>
#include <ccord/core/base64.h>
#include <ccord/core/buf.h>
#include <ccord/core/clock.h>
#include <ccord/core/config.h>
#include <ccord/core/input.h>
#include <ccord/core/json.h>
#include <ccord/core/log.h>
#include <ccord/core/os.h>
#include <ccord/core/paths.h>
#include <ccord/core/utf8.h>
#include <ccord/ipc/ipc.h>
#include <ccord/store/notify.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if !CCORD_OS_WINDOWS
#include <fcntl.h>
#include <signal.h>
#if !CCORD_OS_ANDROID
#include <spawn.h>
#include <sys/wait.h>
#endif
#include <unistd.h>
#endif

const ccord_json_limits pl_json_limits = {16, 8192, (size_t)1u << 20};

void pl_reload_plugin(ccord_plugins *m, const char *id)
{
  pl_on_plugin_reload(m, id);
}

void pl_rm_dir_recursive(const char *path)
{
  ccord_error err = {0};
  (void)ccord_rmtree(path, &err);
}

void pl_set_text(char *dst, size_t cap, const char *src)
{
  if (!dst || cap == 0) return;
  const char *s = src ? src : "";
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1u;
  memcpy(dst, s, n);
  dst[n] = '\0';
}

bool pl_has_perm(const plugin *p, ccord_perm perm)
{
  if ((p->m.perms & (ccord_perm_set)perm) == 0) return false;
  if (ccord_perm_class_of(perm) == CCORD_PERM_CLASS_SAFE) return true;
  return p->consent_granted && (p->consented_perms & (ccord_perm_set)perm) != 0;
}

bool pl_app_raw(ccord_buf *b, const char *s)
{
  return ccord_buf_append_str(b, s) == CCORD_OK;
}

bool pl_app_str(ccord_buf *b, const char *s, size_t n)
{
  ccord_jw w;
  ccord_jw_init(&w, b);
  ccord_jw_string(&w, s, n);
  return ccord_jw_ok(&w);
}

// ---------------------------------------------------------------- validation (main is authoritative)

static bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool pl_valid_store_key(const char *s, size_t n)
{
  if (n == 0 || n > 128) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool pl_valid_menu_id(const char *s, size_t n)
{
  if (n == 0 || n > 32 || !(is_lower(s[0]) || is_digit(s[0]))) return false;
  for (size_t i = 1; i < n; i++) {
    if (!(is_lower(s[i]) || is_digit(s[i]) || s[i] == '_' || s[i] == '-')) return false;
  }
  return true;
}

bool pl_valid_command_name(const char *s, size_t n)
{
  if (n == 0 || n > 32 || !is_lower(s[0])) return false;
  for (size_t i = 1; i < n; i++) {
    if (!(is_lower(s[i]) || is_digit(s[i]) || s[i] == '_' || s[i] == '-')) return false;
  }
  return true;
}

// Valid UTF-8 without control characters (label, description).
bool pl_valid_plain_text(const char *s, size_t n, size_t max)
{
  if (n == 0 || n > max || !ccord_utf8_valid(s, n)) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

// ---------------------------------------------------------------- state publication

void pl_publish_state(ccord_plugins *m, plugin *p)
{
  ccord_catalog_local_overlay ov = {0};
  bool has_ov = m->catalog && ccord_catalog_query_overlay(m->catalog, p->m.id, p->m.version, p->consented_perms, &ov);
  ccord_plugin_tier tier = p->is_bundled ? CCORD_TIER_OFFICIAL : (has_ov ? ov.tier : CCORD_TIER_UNKNOWN);
  ccord_plugin_info info = {
    .id = p->m.id, .name = p->m.name, .version = p->m.version,
    .description = p->m.description,
    .permissions = p->perm_str ? p->perm_str : "",
    .allowed_hosts = p->allowed_hosts_str ? p->allowed_hosts_str : "",
    .perms_mask = p->m.perms,
    .risk_class = (uint8_t)ccord_perm_max_class(p->m.perms),
    .tier = tier,
    .is_bundled = p->is_bundled,
    .auto_update = has_ov ? ov.auto_update : true,
    .enabled = p->enabled,
    .consent_granted = p->consent_granted,
    .update_needs_review = has_ov ? ov.update_needs_review : false,
    .update_version = has_ov ? ov.update_version : "",
    .warning_sev = has_ov ? ov.warning_sev : CCORD_WARN_NONE,
    .warning_title = has_ov ? ov.warning_title : "",
    .warning_message = has_ov ? ov.warning_message : "",
    .state = p->state, .error = p->error,
  };
  ccord_error err = {0};
  if (ccord_contrib_set_plugin(m->contrib, &info, &err) != CCORD_OK) {
    CCORD_LOG_W("plugin %s: contribution registry: %s", p->m.id, err.message);
  }
  if (m->bus) {
    ccord_event ev = {.type = CCORD_EVT_PLUGIN_STATE, .value = (int64_t)p->state};
    (void)ccord_bus_post(m->bus, ev);
  }
}

void pl_set_plugin_failed(ccord_plugins *m, plugin *p, const char *reason, bool allow_from_stopped)
{
  if (p->state == CCORD_PLUGIN_FAILED || (!allow_from_stopped && p->state == CCORD_PLUGIN_STOPPED)) return;
  pl_kill_host_process(p);
  if (p->ipc) {
    ccord_ipc_close(p->ipc);
    p->ipc = nullptr;
  }
  ccord_free(p->source);
  p->source = nullptr;
  p->source_len = 0;
  p->state = CCORD_PLUGIN_FAILED;
  pl_set_text(p->error, sizeof p->error, reason);
  memset(p->pend, 0, sizeof p->pend);
  if (m->workers) pw_remove_owner(m->workers, p->m.id);
  plugins_ext_reset_plugin(p);
  ccord_contrib_remove_owner(m->contrib, p->m.id);
  CCORD_LOG_E("plugin %s FAILED: %s", p->m.id, p->error);
  pl_publish_state(m, p);
  ccord_contrib_add_plugin_log(m->contrib, p->m.id, m->now_ms, (uint8_t)CCORD_LOG_ERROR, p->error);
}

void pl_fail_plugin(ccord_plugins *m, plugin *p, const char *reason)
{
  pl_set_plugin_failed(m, p, reason, false);
}

// Sends a notification; any send failure fails the plugin.
void pl_send_ntf(ccord_plugins *m, plugin *p, const char *method, const char *params)
{
  if (!p->ipc) return;
  ccord_error err = {0};
  if (ccord_ipc_send_notify(p->ipc, method, params, &err) != CCORD_OK) {
    char why[256];
    snprintf(why, sizeof why, "could not send to host: %s", err.message);
    pl_fail_plugin(m, p, why);
    return;
  }
  (void)ccord_ipc_flush(p->ipc, 0, &err);
}

pending_req *pl_pend_slot(plugin *p, pend_kind kind)
{
  for (size_t i = 0; i < MAX_PENDING; i++) {
    if (p->pend[i].kind == kind) return &p->pend[i];
  }
  return nullptr;
}

pending_req *pl_pend_free(plugin *p)
{
  for (size_t i = 0; i < MAX_PENDING; i++) {
    if (p->pend[i].kind == PEND_NONE) return &p->pend[i];
  }
  return nullptr;
}

uint32_t pl_next_request_id(plugin *p)
{
  if (p->next_id == 0) p->next_id = 1;
  return p->next_id++;
}

// ---------------------------------------------------------------- main -> host events and commands

static void bus_send_event(ccord_plugins *m, plugin *p, const char *name, const ccord_buf *data)
{
  ccord_buf params;
  ccord_buf_init(&params, 0);
  bool ok = pl_app_raw(&params, "{\"name\":") && pl_app_str(&params, name, strlen(name)) && pl_app_raw(&params, ",\"data\":") &&
            ccord_buf_append(&params, data->data, data->len) == CCORD_OK && pl_app_raw(&params, "}");
  if (ok) pl_send_ntf(m, p, "event", ccord_buf_cstr(&params));
  ccord_buf_free(&params);
}

static const char *nav_view_str(ccord_nav_view v)
{
  switch (v) {
    case CCORD_NAV_VIEW_FRIENDS: return "friends";
    case CCORD_NAV_VIEW_REQUESTS: return "requests";
    case CCORD_NAV_VIEW_DISCOVERY: return "discovery";
    case CCORD_NAV_VIEW_GUILD: return "guild";
    case CCORD_NAV_VIEW_THREAD: return "thread";
    default: return "channel";
  }
}

static void on_bus_event(const ccord_event *ev, void *ud)
{
  ccord_plugins *m = ud;
  uint64_t bit = 0;
  const char *name = nullptr;
  switch (ev->type) {
    case CCORD_EVT_APP_READY:        bit = EVB_APP_READY;        name = "app.ready"; break;
    case CCORD_EVT_CONNECTION_STATE: bit = EVB_CONNECTION_STATE; name = "connection.state"; break;
    case CCORD_EVT_GUILD_SELECT:     bit = EVB_GUILD;            name = "guild.select"; break;
    case CCORD_EVT_CHANNEL_SELECT:   bit = EVB_CHANNEL;          name = "channel.select"; break;
    case CCORD_EVT_VIEW_SELECT:      bit = EVB_VIEW_SELECT;      name = "view.select"; break;
    case CCORD_EVT_NAV_AUX:          bit = EVB_NAV_AUX;          name = "navigation.aux_open"; break;
    case CCORD_EVT_CHANNEL_UPSERT:   bit = EVB_CHANNEL_UPSERT;   name = "channel.upsert"; break;
    case CCORD_EVT_CHANNEL_DELETE:   bit = EVB_CHANNEL_DELETE;   name = "channel.delete"; break;
    case CCORD_EVT_GUILDS_LOADED:    bit = EVB_GUILDS_LOADED;    name = "guilds.loaded"; break;
    case CCORD_EVT_NOTIFY_UPDATE:    bit = EVB_NOTIFY_UPDATE;    name = "notify.update"; break;
    case CCORD_EVT_MESSAGE_CREATE:   bit = EVB_MESSAGE;          name = "message.create"; break;
    case CCORD_EVT_MESSAGE_UPDATE:   bit = EVB_MESSAGE_UPDATE;   name = "message.update"; break;
    case CCORD_EVT_MESSAGE_DELETE:   bit = EVB_MESSAGE_DELETE;   name = "message.delete"; break;
    case CCORD_EVT_MESSAGE_PIN:      bit = EVB_MESSAGE_PIN;      name = "message.pin"; break;
    case CCORD_EVT_REACTION_UPDATE:  bit = EVB_REACTION_UPDATE;  name = "reaction.update"; break;
    case CCORD_EVT_TYPING_START:     bit = EVB_TYPING_START;     name = "typing.start"; break;
    case CCORD_EVT_PRESENCE_UPDATE:  bit = EVB_PRESENCE_UPDATE;  name = "presence.update"; break;
    case CCORD_EVT_VOICE_STATE:      bit = EVB_VOICE_STATE;      name = "voice.state"; break;
    default: return;
  }

  bool any = false;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    if (p->state == CCORD_PLUGIN_READY && (p->subs & bit)) any = true;
  }
  if (!any) return;

  ccord_buf data;
  ccord_buf_init(&data, 0);
  ccord_jw w;
  ccord_jw_init(&w, &data);
  if (bit == EVB_MESSAGE || bit == EVB_MESSAGE_UPDATE) {
    const ccord_message *msg = m->store ? ccord_store_find_message(m->store, ev->channel_id, ev->message_id) : nullptr;
    const ccord_user *u = msg && m->store ? ccord_store_user(m->store, msg->author_id) : nullptr;
    if (!msg) { ccord_buf_free(&data); return; }
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_id(&w, msg->id);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, msg->channel_id);
    ccord_jw_key(&w, "author_id"); ccord_jw_id(&w, msg->author_id);
    ccord_jw_key(&w, "author_name"); ccord_jw_cstr(&w, u ? u->name : "");
    ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, msg->content ? msg->content : "");
    ccord_jw_key(&w, "timestamp_ms"); ccord_jw_int(&w, msg->timestamp_ms);
    ccord_jw_end_object(&w);
  } else if (bit == EVB_CHANNEL || bit == EVB_CHANNEL_UPSERT) {
    const ccord_channel *c = m->store ? ccord_store_channel(m->store, ev->channel_id) : nullptr;
    if (!c) { ccord_buf_free(&data); return; }
    char dname[192];
    pl_channel_display_name(m->store, c, dname, sizeof dname);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, c->guild_id);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, c->id);
    if (c->parent_id != CCORD_ID_NONE) { ccord_jw_key(&w, "parent_id"); ccord_jw_id(&w, c->parent_id); }
    ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, c->name ? c->name : "");
    ccord_jw_key(&w, "display_name"); ccord_jw_cstr(&w, dname);
    ccord_jw_key(&w, "kind"); ccord_jw_cstr(&w, pl_channel_kind_str(m->store, c));
    ccord_jw_key(&w, "unread_count"); ccord_jw_int(&w, (int64_t)c->unread_count);
    ccord_jw_key(&w, "mention_count"); ccord_jw_int(&w, (int64_t)c->mention_count);
    ccord_jw_end_object(&w);
  } else if (bit == EVB_GUILD) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
    if (ev->guild_id == CCORD_DM_GUILD_ID) {
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, CCORD_DM_GUILD_NAME);
      ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, true);
    } else {
      const ccord_guild *g = m->store ? ccord_store_guild(m->store, ev->guild_id) : nullptr;
      if (!g) { ccord_buf_free(&data); return; }
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, g->name ? g->name : "");
      ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, false);
    }
    ccord_jw_end_object(&w);
  } else if (bit == EVB_VIEW_SELECT) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
    ccord_jw_key(&w, "view"); ccord_jw_cstr(&w, nav_view_str((ccord_nav_view)ev->value));
    ccord_jw_end_object(&w);
  } else if (bit == EVB_NAV_AUX) {
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
    if (ev->channel_id) { ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, ev->channel_id); }
    if (ev->message_id) { ccord_jw_key(&w, "parent_id"); ccord_jw_id(&w, ev->message_id); }
    ccord_jw_key(&w, "view"); ccord_jw_cstr(&w, nav_view_str((ccord_nav_view)ev->value));
    const ccord_channel *c = (m->store && ev->channel_id) ? ccord_store_channel(m->store, ev->channel_id) : nullptr;
    if (c) {
      char dname[192];
      pl_channel_display_name(m->store, c, dname, sizeof dname);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, dname);
    }
    ccord_jw_end_object(&w);
  } else {
    ccord_jw_begin_object(&w);
    if (ev->guild_id) { ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id); }
    if (ev->channel_id) { ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, ev->channel_id); }
    if (ev->message_id) { ccord_jw_key(&w, "message_id"); ccord_jw_id(&w, ev->message_id); }
    if (ev->user_id) { ccord_jw_key(&w, "user_id"); ccord_jw_id(&w, ev->user_id); }
    ccord_jw_key(&w, "value"); ccord_jw_int(&w, ev->value);
    ccord_jw_end_object(&w);
  }
  if (ccord_jw_ok(&w)) {
    for (size_t i = 0; i < m->nplugins; i++) {
      plugin *p = m->plugins[i];
      if (p->state == CCORD_PLUGIN_READY && (p->subs & bit)) bus_send_event(m, p, name, &data);
    }
  }
  ccord_buf_free(&data);
}

plugin *pl_find_owner(ccord_plugins *m, const char *owner)
{
  if (!owner) return nullptr;
  for (size_t i = 0; i < m->nplugins; i++) {
    if (m->plugins[i]->used && strcmp(m->plugins[i]->m.id, owner) == 0) return m->plugins[i];
  }
  return nullptr;
}

static void on_menu_activate(void *ud, const ccord_menu_contribution *item, const ccord_menu_context *ctx)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, item->owner);
  if (!p || p->state != CCORD_PLUGIN_READY) return;
  char g[24] = "", c[24] = "", mid[24] = "", u[24] = "";
  if (ctx->guild_id) ccord_id_format(ctx->guild_id, g);
  if (ctx->channel_id) ccord_id_format(ctx->channel_id, c);
  if (ctx->message_id) ccord_id_format(ctx->message_id, mid);
  if (ctx->user_id) ccord_id_format(ctx->user_id, u);
  const char *kind = pl_menu_target_str(ctx->target);
  ccord_buf params;
  ccord_buf_init(&params, 0);
  ccord_jw w;
  ccord_jw_init(&w, &params);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "item_id"); ccord_jw_cstr(&w, item->item_id);
  ccord_jw_key(&w, "ctx");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "kind"); ccord_jw_cstr(&w, kind);
  if (ctx->guild_id) { ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, g); }
  if (ctx->channel_id) { ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, c); }
  if (ctx->target == CCORD_MENU_NAV) {
    const char *view = ctx->message_id == CCORD_NAV_VIEW_FRIENDS  ? "friends"
                     : ctx->message_id == CCORD_NAV_VIEW_REQUESTS ? "requests"
                     : ctx->message_id == CCORD_NAV_VIEW_GUILD    ? "dm_home" : "nav";
    ccord_jw_key(&w, "view"); ccord_jw_cstr(&w, view);
  } else if (ctx->target == CCORD_MENU_CHANNEL && ctx->message_id) {
    ccord_jw_key(&w, "parent_id"); ccord_jw_cstr(&w, mid);
  } else if (ctx->message_id) {
    ccord_jw_key(&w, "message_id"); ccord_jw_cstr(&w, mid);
  }
  if (ctx->user_id) { ccord_jw_key(&w, "user_id"); ccord_jw_cstr(&w, u); }
  if (ctx->extra_url && ctx->extra_url[0]) { ccord_jw_key(&w, "url"); ccord_jw_cstr(&w, ctx->extra_url); }
  if (ctx->extra_text && ctx->extra_text[0]) { ccord_jw_key(&w, "text"); ccord_jw_cstr(&w, ctx->extra_text); }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "menu.invoke", ccord_buf_cstr(&params));
  else CCORD_LOG_W("plugin %s: menu context not encoded", p->m.id);
  ccord_buf_free(&params);
}

static const char *panel_event_kind_str(ccord_panel_event_kind k)
{
  switch (k) {
    case CCORD_PANEL_EV_CLICK:        return "click";
    case CCORD_PANEL_EV_DOUBLE_CLICK: return "double_click";
    case CCORD_PANEL_EV_MIDDLE_CLICK: return "middle_click";
    case CCORD_PANEL_EV_RIGHT_CLICK:  return "right_click";
    case CCORD_PANEL_EV_WHEEL:        return "wheel";
    case CCORD_PANEL_EV_HOVER_ENTER:  return "hover_enter";
    case CCORD_PANEL_EV_HOVER_LEAVE:  return "hover_leave";
    case CCORD_PANEL_EV_MOUSE_MOVE:   return "mouse_move";
    case CCORD_PANEL_EV_DRAG_START:   return "drag_start";
    case CCORD_PANEL_EV_DRAG_MOVE:    return "drag_move";
    case CCORD_PANEL_EV_DRAG_END:     return "drag_end";
    case CCORD_PANEL_EV_CHANGE:       return "change";
    case CCORD_PANEL_EV_SUBMIT:       return "submit";
    case CCORD_PANEL_EV_FOCUS:        return "focus";
    case CCORD_PANEL_EV_BLUR:         return "blur";
    case CCORD_PANEL_EV_KEY_DOWN:     return "key_down";
    case CCORD_PANEL_EV_CLOSE:        return "close";
    default:                          return "click";
  }
}

static void on_panel_event(void *ud, const ccord_panel_contribution *panel, const ccord_panel_event *ev)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, panel->owner);
  if (!p || p->state != CCORD_PLUGIN_READY) return;
  const char *etype = panel_event_kind_str(ev->kind);
  ccord_buf params;
  ccord_buf_init(&params, 0);
  ccord_jw w;
  ccord_jw_init(&w, &params);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "panel_id"); ccord_jw_cstr(&w, panel->panel_id);
  ccord_jw_key(&w, "event");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "panel_id"); ccord_jw_cstr(&w, panel->panel_id);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, ev->node_id ? ev->node_id : "");
  ccord_jw_key(&w, "type"); ccord_jw_cstr(&w, etype);
  ccord_jw_key(&w, "button"); ccord_jw_int(&w, (int64_t)ev->button);
  ccord_jw_key(&w, "x"); ccord_jw_double(&w, (double)ev->x);
  ccord_jw_key(&w, "y"); ccord_jw_double(&w, (double)ev->y);
  if (ev->kind == CCORD_PANEL_EV_WHEEL) {
    ccord_jw_key(&w, "wheel"); ccord_jw_double(&w, (double)ev->wheel);
  }
  if (ev->text_value) { ccord_jw_key(&w, "value"); ccord_jw_cstr(&w, ev->text_value); }
  else if (ev->has_num) { ccord_jw_key(&w, "value"); ccord_jw_double(&w, ev->num_value); }
  else if (ev->has_bool) { ccord_jw_key(&w, "value"); ccord_jw_bool(&w, ev->bool_value); }
  if (ev->key_name && ev->key_name[0]) { ccord_jw_key(&w, "key"); ccord_jw_cstr(&w, ev->key_name); }
  ccord_jw_key(&w, "mods");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "shift"); ccord_jw_bool(&w, (ev->mods & CCORD_MOD_SHIFT) != 0);
  ccord_jw_key(&w, "ctrl"); ccord_jw_bool(&w, (ev->mods & CCORD_MOD_CTRL) != 0);
  ccord_jw_key(&w, "alt"); ccord_jw_bool(&w, (ev->mods & CCORD_MOD_ALT) != 0);
  ccord_jw_key(&w, "super"); ccord_jw_bool(&w, (ev->mods & CCORD_MOD_SUPER) != 0);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "panel.event", ccord_buf_cstr(&params));
  ccord_buf_free(&params);
}

static void on_command_run(void *ud, const ccord_command_contribution *cmd, const char *args, ccord_id guild_id, ccord_id channel_id)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, cmd->owner);
  if (!p || p->state != CCORD_PLUGIN_READY || !p->ipc) {
    CCORD_LOG_W("command /%s: plugin is not running", cmd->name);
    return;
  }
  pending_req *slot = pl_pend_free(p);
  if (!slot) {
    CCORD_LOG_W("command /%s: too many pending commands", cmd->name);
    return;
  }
  char g[24] = "", c[24] = "";
  if (guild_id) ccord_id_format(guild_id, g);
  if (channel_id) ccord_id_format(channel_id, c);
  ccord_buf params;
  ccord_buf_init(&params, 0);
  ccord_jw w;
  ccord_jw_init(&w, &params);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, cmd->name);
  ccord_jw_key(&w, "args"); ccord_jw_cstr(&w, args ? args : "");
  ccord_jw_key(&w, "ctx");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_cstr(&w, g);
  ccord_jw_key(&w, "channel_id"); ccord_jw_cstr(&w, c);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    uint32_t id = pl_next_request_id(p);
    ccord_error err = {0};
    if (ccord_ipc_send_request(p->ipc, id, "command.run", ccord_buf_cstr(&params), &err) == CCORD_OK) {
      slot->kind = PEND_CMD;
      slot->id = id;
      slot->deadline_ms = m->now_ms + CMD_DEADLINE_MS;
      slot->channel_id = channel_id;
      (void)ccord_ipc_flush(p->ipc, 0, &err);
    } else {
      CCORD_LOG_W("command /%s: not sent: %s", cmd->name, err.message);
    }
  }
  ccord_buf_free(&params);
}

void ccord_plugins_shutdown(ccord_plugins *m)
{
  if (!m) return;
  for (size_t i = 0; i < m->nplugins; i++) pl_stop_host(m, m->plugins[i]);
}

void ccord_plugins_destroy(ccord_plugins *m)
{
  if (!m) return;
  ccord_catalog_destroy(m->catalog);
  m->catalog = nullptr;
  ccord_plugins_shutdown(m);
  if (m->workers) { pw_destroy(m->workers); m->workers = nullptr; }
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    plugins_ext_reset_plugin(p);
    for (size_t k = 0; k < p->nitems; k++) ccord_free(p->items[k].json);
    ccord_free(p->items);
    ccord_free(p->source);
    ccord_free(p->dir);
    ccord_free(p->storage_dir);
    ccord_free(p->perm_str);
    ccord_free(p->allowed_hosts_str);
    ccord_free(p);
  }
  ccord_free(m->plugins);
  if (m->search) {
    for (size_t i = 0; i < m->nsearch; i++) ccord_free(m->search[i]);
    ccord_free(m->search);
  }
  ccord_free(m->host_exe);
  ccord_free(m->data_dir);
  if (m->bus && m->sub) {
    ccord_bus_unsubscribe(m->bus, m->sub);
  }
  ccord_free(m);
}

static void on_plugin_set_enabled(void *ud, const char *id, bool enabled)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, id);
  if (!p) return;
  if (!enabled) {
    p->enabled = false;
    pl_stop_host(m, p);
    p->state = p->revoked ? CCORD_PLUGIN_FAILED
                          : (p->consent_granted ? CCORD_PLUGIN_STOPPED : CCORD_PLUGIN_NEEDS_CONSENT);
    pl_publish_state(m, p);
    pl_consent_save(m);
    return;
  }
  char rev_reason[256] = "";
  if (p->revoked || (m->catalog && ccord_catalog_is_revoked(m->catalog, p->m.id, rev_reason, sizeof rev_reason))) {
    p->revoked = true;
    p->enabled = false;
    p->consent_granted = false;
    p->consented_perms = 0;
    pl_set_plugin_failed(m, p, rev_reason[0] ? rev_reason : "Revoked by C-Cord moderators", true);
    return;
  }
  if (!p->consent_granted) {
    p->state = CCORD_PLUGIN_NEEDS_CONSENT;
    pl_publish_state(m, p);
    return;
  }
  p->enabled = true;
  pl_consent_save(m);
  pl_start_plugin_host(m, p);
}

static void on_plugin_consent_accept_for(void *ud, const char *id, uint64_t expected_perms)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, id);
  if (!p) return;
  char rev_reason[256] = "";
  if (p->revoked || (m->catalog && ccord_catalog_is_revoked(m->catalog, p->m.id, rev_reason, sizeof rev_reason))) {
    p->revoked = true;
    p->enabled = false;
    p->consent_granted = false;
    p->consented_perms = 0;
    pl_set_plugin_failed(m, p, rev_reason[0] ? rev_reason : "Revoked by C-Cord moderators", true);
    return;
  }
  if (expected_perms != UINT64_MAX && p->m.perms != expected_perms) {
    // Manifest permissions changed while the consent dialog was open; keep NEEDS_CONSENT and refresh UI.
    p->consent_granted = false;
    p->enabled = false;
    p->state = CCORD_PLUGIN_NEEDS_CONSENT;
    pl_publish_state(m, p);
    return;
  }
  p->consent_granted = true;
  p->consented_perms = p->m.perms;
  p->enabled = true;
  pl_consent_save(m);
  pl_start_plugin_host(m, p);
}

static void on_plugin_consent_accept(void *ud, const char *id)
{
  on_plugin_consent_accept_for(ud, id, UINT64_MAX);
}

void pl_on_plugin_reload(void *ud, const char *id)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, id);
  if (!p || !p->dir || !p->dir[0]) return;
  bool was_enabled = p->enabled;
  pl_stop_host(m, p);
  char manifest_path[PBUF + 64], main_path[PBUF + 128], why[256];
  snprintf(manifest_path, sizeof manifest_path, "%s/plugin.json", p->dir);
  char *mbuf = nullptr; size_t mlen = 0;
  if (!pl_read_file(manifest_path, 16u << 20, &mbuf, &mlen, why, sizeof why)) {
    pl_set_plugin_failed(m, p, why, true);
    return;
  }
  ccord_manifest man; ccord_error err = {0};
  ccord_status st = ccord_manifest_parse(mbuf, mlen, &man, &err);
  ccord_free(mbuf);
  if (st != CCORD_OK) { pl_set_plugin_failed(m, p, err.message, true); return; }
  if (strcmp(man.id, p->m.id) != 0) {
    pl_set_plugin_failed(m, p, "manifest id changed during reload (rejected)", true);
    return;
  }
  char compat_why[160] = "";
  if (!ccord_manifest_supports(&man, m->plat_bit, m->plat_capabilities, compat_why, sizeof compat_why)) {
    pl_set_plugin_failed(m, p, compat_why[0] ? compat_why : "plugin unsupported on this platform", true);
    return;
  }
  char real_dir[CCORD_PATH_MAX], real_main[CCORD_PATH_MAX];
  if (!pl_resolve_real_path(p->dir, real_dir, sizeof real_dir)) {
    pl_set_plugin_failed(m, p, "plugin directory cannot be resolved", true);
    return;
  }
  int mlen_p = snprintf(main_path, sizeof main_path, "%s/%s", p->dir, man.main);
  if (mlen_p < 0 || (size_t)mlen_p >= sizeof main_path) {
    pl_set_plugin_failed(m, p, "main file path too long", true);
    return;
  }
  if (!pl_resolve_real_path(main_path, real_main, sizeof real_main) || !pl_path_strictly_under(real_main, real_dir)) {
    pl_set_plugin_failed(m, p, "main file resolves outside the plugin directory", true);
    return;
  }
  char *src = nullptr; size_t slen = 0;
  if (!pl_read_file(real_main, 16u << 20, &src, &slen, why, sizeof why)) { pl_set_plugin_failed(m, p, why, true); return; }
  ccord_free(p->source);
  p->source = src;
  p->source_len = slen;
  p->m = man;
  char pbuf[1024] = "";
  ccord_perm_list(man.perms, pbuf, sizeof pbuf);
  char *ps = ccord_strdup(pbuf);
  if (ps) {
    ccord_free(p->perm_str);
    p->perm_str = ps;
  }
  char hbuf[1024] = "";
  plugins_ext_format_allowed_hosts(&man, hbuf, sizeof hbuf);
  char *hs = ccord_strdup(hbuf);
  if (hs) {
    ccord_free(p->allowed_hosts_str);
    p->allowed_hosts_str = hs;
  }

  char rev_reason[256] = "";
  if (m->catalog && ccord_catalog_is_revoked(m->catalog, man.id, rev_reason, sizeof rev_reason)) {
    p->revoked = true;
    p->enabled = false;
    p->consent_granted = false;
    p->consented_perms = 0;
    pl_set_plugin_failed(m, p, rev_reason[0] ? rev_reason : "Revoked by C-Cord moderators", true);
    pl_consent_save(m);
    return;
  }
  p->revoked = false;

  bool safe_class = ccord_perm_max_class(man.perms) == CCORD_PERM_CLASS_SAFE;
  if (safe_class) {
    p->consent_granted = true;
    p->consented_perms = man.perms;
  } else if (!p->consent_granted || (man.perms & ~p->consented_perms) != 0) {
    ccord_free(p->source);
    p->source = nullptr;
    p->source_len = 0;
    p->consent_granted = false;
    p->enabled = false;
    p->state = CCORD_PLUGIN_NEEDS_CONSENT;
    pl_publish_state(m, p);
    pl_consent_save(m);
    return;
  }

  p->enabled = was_enabled;
  pl_consent_save(m);
  if (p->enabled) {
    pl_start_plugin_host(m, p);
  } else {
    ccord_free(p->source);
    p->source = nullptr;
    p->source_len = 0;
    p->state = CCORD_PLUGIN_STOPPED;
    pl_publish_state(m, p);
  }
}

static bool cb_export_cce(void *ud, const char *id, const char *dest, char *eb, size_t ec)
{
  return ccord_plugins_export_cce(ud, id, dest, eb, ec);
}

static bool cb_import_pkg(void *ud, const char *pkg, char *eb, size_t ec)
{
  return ccord_plugins_import_package(ud, pkg, eb, ec);
}

static bool cb_import_verified_cce(void *ud, const char *pkg, const char *expected_id,
                                   const char *expected_version, uint64_t expected_perms,
                                   char *eb, size_t ec)
{
  return pl_import_package_internal(ud, pkg, expected_id, expected_version, expected_perms, true, eb, ec);
}

static bool cb_uninstall(void *ud, const char *id, char *eb, size_t ec)
{
  return ccord_plugins_uninstall(ud, id, eb, ec);
}

static void cb_cat_refresh(void *ud) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_refresh(m->catalog); }
static void cb_cat_query(void *ud, const char *q, int tier, int api_class, const char *sort, bool append_next_page)
{
  ccord_plugins *m = ud;
  if (m->catalog) ccord_catalog_query(m->catalog, q, tier, api_class, sort, append_next_page);
}
static void cb_cat_fetch_detail(void *ud, const char *id)
{
  ccord_plugins *m = ud;
  if (m->catalog) ccord_catalog_fetch_detail(m->catalog, id);
}
static void cb_cat_check_updates(void *ud) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_check_updates(m->catalog); }
static void cb_cat_install(void *ud, const char *id) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_install(m->catalog, id); }
static void cb_cat_set_prefs(void *ud, const ccord_catalog_prefs *pr) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_set_prefs(m->catalog, pr); }
static void cb_cat_set_auto(void *ud, const char *id, bool on) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_set_plugin_auto_update(m->catalog, id, on); }
static void cb_cat_run_auto(void *ud) { ccord_plugins *m = ud; if (m->catalog) ccord_catalog_run_auto_updates(m->catalog); }

static bool cat_get_local_plugin(void *ud, const char *id, char *ver_out, size_t ver_cap,
                                 uint64_t *consented_perms_out, bool *is_bundled_out)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, id);
  if (!p) return false;
  pl_set_text(ver_out, ver_cap, p->m.version);
  if (consented_perms_out) *consented_perms_out = p->consented_perms;
  if (is_bundled_out) *is_bundled_out = p->is_bundled;
  return true;
}

static size_t cat_list_local_plugins(void *ud, char (*ids_out)[65], size_t cap)
{
  ccord_plugins *m = ud;
  size_t n = 0;
  for (size_t i = 0; i < m->nplugins && n < cap; i++) {
    const plugin *p = m->plugins[i];
    if (p->used && !p->is_bundled && p->m.id[0]) {
      pl_set_text(ids_out[n++], 65, p->m.id);
    }
  }
  return n;
}

static void cat_revoke_plugin(void *ud, const char *id, const char *reason)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, id);
  if (!p) return;
  pl_stop_host(m, p);
  p->revoked = true;
  p->enabled = false;
  p->consent_granted = false;
  p->consented_perms = 0;
  p->state = CCORD_PLUGIN_FAILED;
  pl_set_text(p->error, sizeof p->error, reason ? reason : "Revoked by C-Cord moderators");
  pl_consent_save(m);
  pl_publish_state(m, p);
}

static void cat_sync_installed_info(void *ud)
{
  ccord_plugins *m = ud;
  for (size_t i = 0; i < m->nplugins; i++) {
    if (m->plugins[i]->used) pl_publish_state(m, m->plugins[i]);
  }
}

static void cat_toast(void *ud, const char *msg, uint8_t level)
{
  ccord_plugins *m = ud;
  if (m->toast_show) m->toast_show(m->toast_ud, msg, level);
}

ccord_plugins *ccord_plugins_create(const ccord_plugins_config *cfg, ccord_error *err)
{
  if (!cfg || !cfg->bus || !cfg->store || !cfg->contrib ||
      (!cfg->in_process && !cfg->host_exe) ||
      (cfg->in_process && !cfg->in_process_host_fn)) {
    ccord_error_set(err, CCORD_ERR_INVALID, "plugins: bus, store, contrib and host_exe (or in_process_host_fn) are required");
    return nullptr;
  }
  if (cfg->search_dir_count > MAX_SEARCH) {
    ccord_error_set(err, CCORD_ERR_RANGE, "plugins: too many search directories");
    return nullptr;
  }
  ccord_plugins *m = ccord_calloc(1, sizeof *m);
  if (!m) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "plugins: out of memory");
    return nullptr;
  }
  m->bus = cfg->bus;
  m->store = cfg->store;
  m->session = cfg->session;
  m->contrib = cfg->contrib;
  m->clipboard_write = cfg->clipboard_write;
  m->clipboard_ud = cfg->clipboard_ud;
  m->toast_show = cfg->toast_show;
  m->toast_ud = cfg->toast_ud;
  m->in_process = cfg->in_process;
  m->in_process_host_fn = cfg->in_process_host_fn;
  m->plat_ops = cfg->platform_ops;
  m->dev_auto_reload = cfg->dev_auto_reload;
  m->workers = pw_create(&m->plat_ops);
  m->plat_bit = ccord_platform_current_bit();
  if (cfg->has_platform_info) {
    snprintf(m->plat_os, sizeof m->plat_os, "%s",
             (cfg->platform_info.os && cfg->platform_info.os[0]) ? cfg->platform_info.os : ccord_os_name());
    snprintf(m->plat_arch, sizeof m->plat_arch, "%s",
             (cfg->platform_info.arch && cfg->platform_info.arch[0]) ? cfg->platform_info.arch : ccord_os_arch());
    snprintf(m->plat_form_factor, sizeof m->plat_form_factor, "%s",
             (cfg->platform_info.form_factor && cfg->platform_info.form_factor[0])
               ? cfg->platform_info.form_factor
               : (cfg->platform_info.is_mobile ? "phone" : "desktop"));
    m->plat_is_mobile = cfg->platform_info.is_mobile;
    m->plat_touch = cfg->platform_info.touch;
    m->plat_android_api_level = cfg->platform_info.android_api_level;
    m->plat_capabilities = cfg->platform_info.capabilities;
    m->plat_state = cfg->platform_state;
    if (strcmp(m->plat_os, "android") == 0) m->plat_bit = CCORD_PLAT_ANDROID;
    else if (strcmp(m->plat_os, "macos") == 0) m->plat_bit = CCORD_PLAT_MACOS;
    else if (strcmp(m->plat_os, "windows") == 0) m->plat_bit = CCORD_PLAT_WINDOWS;
    else if (strcmp(m->plat_os, "linux") == 0) m->plat_bit = CCORD_PLAT_LINUX;
  } else {
    snprintf(m->plat_os, sizeof m->plat_os, "%s", ccord_os_name());
    snprintf(m->plat_arch, sizeof m->plat_arch, "%s", ccord_os_arch());
    m->plat_is_mobile = ccord_os_is_mobile();
    snprintf(m->plat_form_factor, sizeof m->plat_form_factor, "%s", m->plat_is_mobile ? "phone" : "desktop");
    m->plat_touch = m->plat_is_mobile;
    m->plat_android_api_level = ccord_os_android_api_level();
    m->plat_capabilities = CCORD_CAP_PANELS | (m->plat_is_mobile ? (CCORD_CAP_TOUCH | CCORD_CAP_HAPTICS | CCORD_CAP_SHARE | CCORD_CAP_SAFE_AREA | CCORD_CAP_KEYBOARD_CONTROL | CCORD_CAP_ORIENTATION_CONTROL | CCORD_CAP_KEEP_SCREEN_ON | CCORD_CAP_NOTIFICATIONS)
                                                                 : (CCORD_CAP_HOVER | CCORD_CAP_NOTIFICATIONS | CCORD_CAP_TRAY | CCORD_CAP_GLOBAL_SHORTCUTS | CCORD_CAP_SCREEN_CAPTURE | CCORD_CAP_RPC | CCORD_CAP_KEEP_SCREEN_ON));
    m->plat_state.viewport_scale = 1.0f;
    m->plat_state.landscape = !m->plat_is_mobile;
  }
  if (cfg->host_exe) {
    if (strlen(cfg->host_exe) >= CCORD_PATH_MAX) {
      ccord_error_set(err, CCORD_ERR_RANGE, "plugins: host path too long");
      ccord_plugins_destroy(m);
      return nullptr;
    }
    m->host_exe = ccord_strdup(cfg->host_exe);
    if (!m->host_exe) {
      ccord_error_set(err, CCORD_ERR_NOMEM, "plugins: out of memory");
      ccord_plugins_destroy(m);
      return nullptr;
    }
  }
  if (cfg->data_dir) {
    if (strlen(cfg->data_dir) < CCORD_PATH_MAX) {
      m->data_dir = ccord_strdup(cfg->data_dir);
      m->has_data_dir = m->data_dir != nullptr;
    }
  }
  if (cfg->search_dir_count > 0) {
    m->search = ccord_calloc(cfg->search_dir_count, sizeof *m->search);
    if (!m->search) {
      ccord_error_set(err, CCORD_ERR_NOMEM, "plugins: out of memory");
      ccord_plugins_destroy(m);
      return nullptr;
    }
    for (size_t i = 0; i < cfg->search_dir_count; i++) {
      if (!cfg->search_dirs[i] || strlen(cfg->search_dirs[i]) >= CCORD_PATH_MAX) {
        ccord_error_set(err, CCORD_ERR_RANGE, "plugins: search directory too long");
        ccord_plugins_destroy(m);
        return nullptr;
      }
      m->search[i] = ccord_strdup(cfg->search_dirs[i]);
      if (!m->search[i]) {
        ccord_error_set(err, CCORD_ERR_NOMEM, "plugins: out of memory");
        ccord_plugins_destroy(m);
        return nullptr;
      }
      m->nsearch++;
    }
  }
  ccord_contrib_handlers h = {
    .ud = m,
    .menu_activate = on_menu_activate,
    .command_run = on_command_run,
    .command_complete = plugins_ext_on_command_complete,
    .panel_event = on_panel_event,
    .decor_event = plugins_ext_on_decor_event,
    .plugin_setting_changed = plugins_ext_on_setting_changed,
    .keybind_triggered = plugins_ext_on_keybind_triggered,
    .action_btn_click = plugins_ext_on_action_btn_click,
    .dialog_result = plugins_ext_on_dialog_result,
    .autocomplete_query = plugins_ext_on_autocomplete_query,
    .before_send_hook = plugins_ext_on_before_send,
    .before_edit_hook = plugins_ext_on_before_edit,
    .notification_hook = plugins_ext_on_notification_hook,
    .link_open_hook = plugins_ext_on_link_open_hook,
    .plugin_set_enabled = on_plugin_set_enabled,
    .plugin_consent_accept = on_plugin_consent_accept,
    .plugin_consent_accept_for = on_plugin_consent_accept_for,
    .plugin_reload = pl_on_plugin_reload,
    .plugin_export_cce = cb_export_cce,
    .plugin_import_package = cb_import_pkg,
    .plugin_uninstall = cb_uninstall,
    .catalog_refresh = cb_cat_refresh,
    .catalog_query = cb_cat_query,
    .catalog_fetch_detail = cb_cat_fetch_detail,
    .catalog_check_updates = cb_cat_check_updates,
    .catalog_install = cb_cat_install,
    .catalog_set_prefs = cb_cat_set_prefs,
    .catalog_set_plugin_auto_update = cb_cat_set_auto,
    .catalog_run_auto_updates = cb_cat_run_auto,
  };
  ccord_contrib_set_handlers(m->contrib, &h);
  ccord_catalog_host_cbs ccbs = {
    .ud = m,
    .get_local_plugin = cat_get_local_plugin,
    .list_local_plugins = cat_list_local_plugins,
    .import_verified_cce = cb_import_verified_cce,
    .revoke_plugin = cat_revoke_plugin,
    .sync_installed_info = cat_sync_installed_info,
    .toast = cat_toast,
  };
  m->catalog = ccord_catalog_create(m->has_data_dir ? m->data_dir : nullptr, m->contrib, &cfg->catalog_http, &ccbs);
  m->sub = ccord_bus_subscribe(m->bus, CCORD_EVT_ANY, on_bus_event, m);
  if (m->sub == 0) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "plugins: bus subscription failed");
    ccord_plugins_destroy(m);
    return nullptr;
  }
  return m;
}

int ccord_plugins_host_pid(const ccord_plugins *m, const char *owner)
{
  if (!m || !owner) return -1;
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = m->plugins[i];
    if (p->used && strcmp(p->m.id, owner) == 0) {
      if (!p->ipc || !pl_host_is_running(p)) return -1;
      if (p->in_process) return (int)ccord_os_pid();
#if CCORD_OS_WINDOWS
      return (int)GetProcessId(p->proc);
#elif CCORD_OS_ANDROID
      return (int)ccord_os_pid();
#else
      return (int)p->pid;
#endif
    }
  }
  return -1;
}

void pl_broadcast_event_json(ccord_plugins *m, uint64_t bit, const char *name, const char *data_json)
{
  if (!m || !name || !data_json) return;
  ccord_buf b;
  ccord_buf_init(&b, 64u + strlen(data_json));
  bool ok = pl_app_raw(&b, "{\"name\":") && pl_app_str(&b, name, strlen(name)) && pl_app_raw(&b, ",\"data\":") &&
            pl_app_raw(&b, data_json) && pl_app_raw(&b, "}");
  if (ok) {
    for (size_t i = 0; i < m->nplugins; i++) {
      plugin *p = m->plugins[i];
      if (!p->used || p->state != CCORD_PLUGIN_READY || !p->ipc) continue;
      if ((p->subs & bit) == 0) continue;
      ccord_error err = {0};
      (void)ccord_ipc_send_notify(p->ipc, "event", ccord_buf_cstr(&b), &err);
    }
  }
  ccord_buf_free(&b);
}

void ccord_plugins_update_platform_state(ccord_plugins *m, const ccord_plugin_platform_state *st)
{
  if (!m || !st) return;
  ccord_plugin_platform_state prev = m->plat_state;
  m->plat_state = *st;
  if (m->nplugins == 0) return;

  bool any_ready = false;
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = m->plugins[i];
    if (p->used && p->state == CCORD_PLUGIN_READY && p->ipc) {
      any_ready = true;
      break;
    }
  }
  if (!any_ready) return;

  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "safe_top"); ccord_jw_int(&w, (int64_t)st->safe_top);
  ccord_jw_key(&w, "safe_right"); ccord_jw_int(&w, (int64_t)st->safe_right);
  ccord_jw_key(&w, "safe_bottom"); ccord_jw_int(&w, (int64_t)st->safe_bottom);
  ccord_jw_key(&w, "safe_left"); ccord_jw_int(&w, (int64_t)st->safe_left);
  ccord_jw_key(&w, "viewport_w"); ccord_jw_int(&w, (int64_t)st->viewport_w);
  ccord_jw_key(&w, "viewport_h"); ccord_jw_int(&w, (int64_t)st->viewport_h);
  ccord_jw_key(&w, "viewport_scale"); ccord_jw_double(&w, (double)(st->viewport_scale > 0.0f ? st->viewport_scale : 1.0f));
  ccord_jw_key(&w, "keyboard_height"); ccord_jw_int(&w, (int64_t)st->keyboard_height);
  ccord_jw_key(&w, "keyboard_visible"); ccord_jw_bool(&w, st->keyboard_visible);
  ccord_jw_key(&w, "landscape"); ccord_jw_bool(&w, st->landscape);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) {
    for (size_t i = 0; i < m->nplugins; i++) {
      plugin *p = m->plugins[i];
      if (!p->used || p->state != CCORD_PLUGIN_READY || !p->ipc) continue;
      ccord_error err = {0};
      (void)ccord_ipc_send_notify(p->ipc, "platform.state", ccord_buf_cstr(&b), &err);
    }
  }
  ccord_buf_free(&b);

  if (prev.landscape != st->landscape) {
    char dj[160];
    snprintf(dj, sizeof dj, "{\"orientation\":\"%s\",\"width\":%d,\"height\":%d}",
             st->landscape ? "landscape" : "portrait", (int)st->viewport_w, (int)st->viewport_h);
    pl_broadcast_event_json(m, EVB_PLAT_ORIENTATION, "platform.orientation", dj);
  }
  if (prev.keyboard_visible != st->keyboard_visible || prev.keyboard_height != st->keyboard_height) {
    char dj[128];
    snprintf(dj, sizeof dj, "{\"visible\":%s,\"height\":%d}",
             st->keyboard_visible ? "true" : "false", (int)st->keyboard_height);
    pl_broadcast_event_json(m, EVB_PLAT_KEYBOARD, "platform.keyboard", dj);
  }
  if (prev.safe_top != st->safe_top || prev.safe_right != st->safe_right ||
      prev.safe_bottom != st->safe_bottom || prev.safe_left != st->safe_left) {
    char dj[160];
    snprintf(dj, sizeof dj, "{\"top\":%d,\"right\":%d,\"bottom\":%d,\"left\":%d}",
             (int)st->safe_top, (int)st->safe_right, (int)st->safe_bottom, (int)st->safe_left);
    pl_broadcast_event_json(m, EVB_PLAT_SAFE_AREA, "platform.safe_area", dj);
  }
}

void ccord_plugins_dispatch_lifecycle(ccord_plugins *m, const char *state)
{
  if (!m || !state || !state[0]) return;
  char dj[96];
  snprintf(dj, sizeof dj, "{\"state\":\"%.32s\"}", state);
  pl_broadcast_event_json(m, EVB_APP_LIFECYCLE, "app.lifecycle", dj);
}

