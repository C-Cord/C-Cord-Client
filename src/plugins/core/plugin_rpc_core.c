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


void pl_jw_copy_json(ccord_jw *w, const ccord_json *j)
{
  ccord_jw_copy_json(w, j);
}

void pl_fs_b64_encode(const unsigned char *in, size_t n, ccord_buf *out)
{
  if (n == 0) return;
  size_t elen = 0;
  char *enc = ccord_base64_encode_alloc(in, n, &elen);
  if (enc) {
    (void)ccord_buf_append(out, enc, elen);
    ccord_free(enc);
  }
}

bool pl_fs_b64_decode(const unsigned char *in, size_t n, uint8_t *dst, size_t *out_len)
{
  if (n % 4u != 0) return false;
  if (n == 0) {
    *out_len = 0;
    return true;
  }
  size_t cap = (n / 4u) * 3u;
  return ccord_base64_decode((const char *)in, n, dst, cap, out_len);
}

// ---------------------------------------------------------------- host -> main requests

void pl_reply_err(ccord_plugins *m, plugin *p, uint32_t id, const char *code, const char *msg)
{
  if (!p->ipc) return;
  ccord_error err = {0};
  if (ccord_ipc_send_error(p->ipc, id, code, msg, &err) != CCORD_OK) pl_fail_plugin(m, p, "could not reply to host");
}

void pl_reply_ok(ccord_plugins *m, plugin *p, uint32_t id, const char *result)
{
  if (!p->ipc) return;
  ccord_error err = {0};
  if (ccord_ipc_send_ok(p->ipc, id, result, &err) != CCORD_OK) pl_fail_plugin(m, p, "could not reply to host");
}

static bool event_bit_for(const char *name, uint64_t *bit, ccord_perm *perm)
{
  // Class 1 (Safe) events
  if (strcmp(name, "app.ready") == 0) { *bit = EVB_APP_READY; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "app.quit") == 0) { *bit = EVB_APP_QUIT; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "app.lifecycle") == 0) { *bit = EVB_APP_LIFECYCLE; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "connection.state") == 0) { *bit = EVB_CONNECTION_STATE; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "guild.select") == 0) { *bit = EVB_GUILD; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "channel.select") == 0) { *bit = EVB_CHANNEL; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "view.select") == 0) { *bit = EVB_VIEW_SELECT; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "navigation.aux_open") == 0) { *bit = EVB_NAV_AUX; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "platform.orientation") == 0) { *bit = EVB_PLAT_ORIENTATION; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "platform.keyboard") == 0) { *bit = EVB_PLAT_KEYBOARD; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "platform.safe_area") == 0) { *bit = EVB_PLAT_SAFE_AREA; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }

  // Class 2 (Unsafe) events
  if (strcmp(name, "message.create") == 0) { *bit = EVB_MESSAGE; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "message.update") == 0) { *bit = EVB_MESSAGE_UPDATE; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "message.delete") == 0) { *bit = EVB_MESSAGE_DELETE; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "message.pin") == 0) { *bit = EVB_MESSAGE_PIN; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "reaction.update") == 0) { *bit = EVB_REACTION_UPDATE; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "typing.start") == 0) { *bit = EVB_TYPING_START; *perm = CCORD_PERM_EVENTS_MESSAGES; return true; }
  if (strcmp(name, "presence.update") == 0) { *bit = EVB_PRESENCE_UPDATE; *perm = CCORD_PERM_EVENTS_PRESENCE; return true; }
  if (strcmp(name, "voice.state") == 0) { *bit = EVB_VOICE_STATE; *perm = CCORD_PERM_EVENTS_VOICE; return true; }
  if (strcmp(name, "voice.server") == 0) { *bit = EVB_VOICE_SERVER; *perm = CCORD_PERM_EVENTS_VOICE; return true; }
  if (strcmp(name, "stream.state") == 0) { *bit = EVB_STREAM_STATE; *perm = CCORD_PERM_EVENTS_VOICE; return true; }
  if (strcmp(name, "channel.upsert") == 0) { *bit = EVB_CHANNEL_UPSERT; *perm = CCORD_PERM_EVENTS_CHANNELS; return true; }
  if (strcmp(name, "channel.delete") == 0) { *bit = EVB_CHANNEL_DELETE; *perm = CCORD_PERM_EVENTS_CHANNELS; return true; }
  if (strcmp(name, "guilds.loaded") == 0) { *bit = EVB_GUILDS_LOADED; *perm = CCORD_PERM_EVENTS_CHANNELS; return true; }
  if (strcmp(name, "notify.update") == 0) { *bit = EVB_NOTIFY_UPDATE; *perm = CCORD_PERM_EVENTS_CHANNELS; return true; }
  return plugins_ext_event_bit_for(name, bit, perm);
}

const char *pl_str_param(const ccord_json *obj, const char *key, size_t *len)
{
  const ccord_json *j = obj ? ccord_json_get(obj, key) : nullptr;
  return j ? ccord_json_string(j, len) : nullptr;
}

static void req_storage(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params)
{
  ccord_buf out;
  ccord_buf_init(&out, 0);
  size_t kl = 0;
  const char *key = pl_str_param(params, "key", &kl);
  if (strcmp(method, "storage.keys") == 0) {
    // Sorted key list.
    size_t n = p->nitems;
    const char **names = ccord_calloc(n ? n : 1, sizeof *names);
    if (!names) {
      pl_reply_err(m, p, id, "internal", "out of memory");
      ccord_buf_free(&out);
      return;
    }
    for (size_t i = 0; i < n; i++) names[i] = p->items[i].key;
    for (size_t i = 1; i < n; i++) {   // insertion sort: at most 128 keys
      const char *x = names[i];
      size_t j = i;
      while (j > 0 && strcmp(names[j - 1], x) > 0) {
        names[j] = names[j - 1];
        j--;
      }
      names[j] = x;
    }
    bool ok = pl_app_raw(&out, "{\"keys\":[");
    for (size_t i = 0; ok && i < n; i++) {
      if (i) ok = pl_app_raw(&out, ",");
      ok = ok && pl_app_str(&out, names[i], strlen(names[i]));
    }
    ok = ok && pl_app_raw(&out, "]}");
    if (ok) pl_reply_ok(m, p, id, ccord_buf_cstr(&out));
    else pl_reply_err(m, p, id, "internal", "could not encode keys");
    ccord_free(names);
    ccord_buf_free(&out);
    return;
  }
  if (strcmp(method, "storage.clear") == 0) {
    for (size_t i = 0; i < p->nitems; i++) ccord_free(p->items[i].json);
    p->nitems = 0;
    p->store_bytes = 0;
    p->dirty = true;
    pl_reply_ok(m, p, id, nullptr);
    ccord_buf_free(&out);
    return;
  }
  if (strcmp(method, "storage.set") == 0) {
    const ccord_json *vj = params ? ccord_json_get(params, "value") : nullptr;
    if (!key || !pl_valid_store_key(key, kl)) {
      pl_reply_err(m, p, id, "invalid_argument", "storage key must match ^[A-Za-z0-9_.-]{1,128}$");
    } else if (!vj) {
      pl_reply_err(m, p, id, "invalid_argument", "storage value is required");
    } else {
      ccord_jw w;
      ccord_jw_init(&w, &out);
      pl_jw_copy_json(&w, vj);
      if (!ccord_jw_ok(&w)) {
        pl_reply_err(m, p, id, "invalid_argument", "storage value could not be encoded");
      } else if (pl_store_put(p, key, ccord_buf_cstr(&out), out.len) != CCORD_OK) {
        pl_reply_err(m, p, id, "internal", "out of memory");
      } else {
        pl_reply_ok(m, p, id, nullptr);
      }
    }
    ccord_buf_free(&out);
    return;
  }
  if (!key || !pl_valid_store_key(key, kl)) {
    pl_reply_err(m, p, id, "invalid_argument", "storage key must match ^[A-Za-z0-9_.-]{1,64}$");
    ccord_buf_free(&out);
    return;
  }
  stored_value *v = pl_store_find(p, key);
  if (strcmp(method, "storage.get") == 0) {
    bool ok = v ? (pl_app_raw(&out, "{\"found\":true,\"value\":") &&
                   ccord_buf_append(&out, v->json, v->json_len) == CCORD_OK && pl_app_raw(&out, "}"))
                : pl_app_raw(&out, "{\"found\":false}");
    if (ok) pl_reply_ok(m, p, id, ccord_buf_cstr(&out));
    else pl_reply_err(m, p, id, "internal", "could not encode value");
  } else {   // storage.delete
    bool existed = v != nullptr;
    if (v) {
      size_t i = (size_t)(v - p->items);
      pl_store_remove_at(p, i);
      p->dirty = true;
    }
    pl_reply_ok(m, p, id, existed ? "{\"existed\":true}" : "{\"existed\":false}");
  }
  ccord_buf_free(&out);
}

const char *pl_menu_target_str(ccord_menu_target t)
{
  switch (t) {
    case CCORD_MENU_MESSAGE:       return "message";
    case CCORD_MENU_CHANNEL:       return "channel";
    case CCORD_MENU_GUILD:         return "guild";
    case CCORD_MENU_MEMBER:        return "member";
    case CCORD_MENU_NAV:           return "nav";
    case CCORD_MENU_PROFILE:       return "profile";
    case CCORD_MENU_ATTACHMENT:    return "attachment";
    case CCORD_MENU_EMBED:         return "embed";
    case CCORD_MENU_LINK:          return "link";
    case CCORD_MENU_VOICE_CHANNEL: return "voice_channel";
    case CCORD_MENU_VOICE_USER:    return "voice_user";
    case CCORD_MENU_COMPOSER:      return "composer";
    default:                       return "message";
  }
}

bool pl_parse_menu_target(const char *s, ccord_menu_target *out)
{
  if (!s) return false;
  static const struct { const char *name; ccord_menu_target t; } k_map[] = {
    {"message", CCORD_MENU_MESSAGE}, {"channel", CCORD_MENU_CHANNEL}, {"guild", CCORD_MENU_GUILD},
    {"member", CCORD_MENU_MEMBER}, {"nav", CCORD_MENU_NAV}, {"profile", CCORD_MENU_PROFILE},
    {"attachment", CCORD_MENU_ATTACHMENT}, {"embed", CCORD_MENU_EMBED}, {"link", CCORD_MENU_LINK},
    {"voice_channel", CCORD_MENU_VOICE_CHANNEL}, {"voice_user", CCORD_MENU_VOICE_USER},
    {"composer", CCORD_MENU_COMPOSER},
  };
  for (size_t i = 0; i < sizeof k_map / sizeof k_map[0]; i++) {
    if (strcmp(s, k_map[i].name) == 0) { *out = k_map[i].t; return true; }
  }
  return false;
}

static void req_menu_add(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0, ll = 0, tl = 0, icl = 0, sml = 0;
  const char *item = pl_str_param(params, "id", &il);
  const char *label = pl_str_param(params, "label", &ll);
  const char *target = pl_str_param(params, "target", &tl);
  if (!item || !pl_valid_menu_id(item, il)) { pl_reply_err(m, p, id, "invalid_argument", "menu id must match ^[a-z0-9][a-z0-9_-]{0,31}$"); return; }
  if (!label || !pl_valid_plain_text(label, ll, 64)) { pl_reply_err(m, p, id, "invalid_argument", "label must be 1-64 bytes of text"); return; }
  ccord_menu_target t = CCORD_MENU_MESSAGE;
  if (!target || !pl_parse_menu_target(target, &t)) {
    pl_reply_err(m, p, id, "invalid_argument", "unknown context menu target");
    return;
  }
  char item_id[40], label_s[80];
  snprintf(item_id, sizeof item_id, "%.*s", (int)il, item);
  snprintf(label_s, sizeof label_s, "%.*s", (int)ll, label);
  const char *icon = pl_str_param(params, "icon", &icl);
  const char *submenu = pl_str_param(params, "submenu", &sml);
  ccord_buf when_b; ccord_buf_init(&when_b, 0);
  const ccord_json *wj = params ? ccord_json_get(params, "when") : nullptr;
  if (wj) { ccord_jw jw; ccord_jw_init(&jw, &when_b); pl_jw_copy_json(&jw, wj); }
  ccord_menu_contribution spec = {
    .owner = p->m.id, .item_id = item_id, .label = label_s, .target = t,
    .icon = icon ? icon : "", .submenu = submenu ? submenu : "",
    .when_json = when_b.len ? ccord_buf_cstr(&when_b) : "",
    .danger = params && ccord_json_bool(ccord_json_get(params, "danger"), false),
    .checked = params && ccord_json_bool(ccord_json_get(params, "checked"), false),
    .divider_before = params && ccord_json_bool(ccord_json_get(params, "divider_before"), false),
    .has_resolver = params && ccord_json_bool(ccord_json_get(params, "has_resolver"), false),
  };
  ccord_error err = {0};
  ccord_status st = ccord_contrib_add_menu_ex(m->contrib, &spec, &err);
  ccord_buf_free(&when_b);
  if (st != CCORD_OK) { pl_reply_err(m, p, id, "invalid_argument", err.message); return; }
  p->menu_count++;
  pl_reply_ok(m, p, id, nullptr);
}

static bool parse_menu_state_target_id(const char *s, size_t n, ccord_id *out)
{
  *out = CCORD_ID_NONE;
  if (!s || n == 0 || (n == 1 && s[0] == '*')) return true;
  if (n == 7 && memcmp(s, "friends", 7) == 0) { *out = CCORD_NAV_VIEW_FRIENDS; return true; }
  if (n == 8 && memcmp(s, "requests", 8) == 0) { *out = CCORD_NAV_VIEW_REQUESTS; return true; }
  if (n == 7 && memcmp(s, "dm_home", 7) == 0) { *out = CCORD_NAV_VIEW_GUILD; return true; }
  return ccord_id_parse(s, n, out);
}

static void req_menu_set_state(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0;
  const char *item = pl_str_param(params, "id", &il);
  if (!item || !pl_valid_menu_id(item, il)) {
    pl_reply_err(m, p, id, "invalid_argument", "menu id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  char item_id[40];
  snprintf(item_id, sizeof item_id, "%.*s", (int)il, item);

  ccord_id tid = CCORD_ID_NONE;
  const ccord_json *tj = params ? ccord_json_get(params, "target_id") : nullptr;
  if (tj && ccord_json_type_of(tj) != CCORD_JSON_NULL) {
    size_t tl = 0;
    const char *ts = ccord_json_string(tj, &tl);
    if (!ts || !parse_menu_state_target_id(ts, tl, &tid)) {
      pl_reply_err(m, p, id, "invalid_argument", "invalid target_id");
      return;
    }
  }

  const ccord_json *sj = params ? ccord_json_get(params, "state") : nullptr;
  if (!sj || ccord_json_type_of(sj) != CCORD_JSON_OBJECT) {
    pl_reply_err(m, p, id, "invalid_argument", "state must be an object");
    return;
  }
  ccord_menu_item_state st = {0};
  const ccord_json *lj = ccord_json_get(sj, "label");
  if (lj && ccord_json_type_of(lj) == CCORD_JSON_STRING) {
    size_t ll = 0;
    const char *ls = ccord_json_string(lj, &ll);
    if (!ls || !pl_valid_plain_text(ls, ll, 64)) {
      pl_reply_err(m, p, id, "invalid_argument", "label must be 1-64 bytes of text");
      return;
    }
    st.has_label = true;
    snprintf(st.label, sizeof st.label, "%.*s", (int)ll, ls);
  }
  const ccord_json *cj = ccord_json_get(sj, "checked");
  if (cj && ccord_json_type_of(cj) == CCORD_JSON_BOOL) { st.has_checked = true; st.checked = ccord_json_bool(cj, false); }
  const ccord_json *hj = ccord_json_get(sj, "hidden");
  if (hj && ccord_json_type_of(hj) == CCORD_JSON_BOOL) { st.has_hidden = true; st.hidden = ccord_json_bool(hj, false); }
  const ccord_json *dj = ccord_json_get(sj, "disabled");
  if (dj && ccord_json_type_of(dj) == CCORD_JSON_BOOL) { st.has_disabled = true; st.disabled = ccord_json_bool(dj, false); }
  const ccord_json *rj = ccord_json_get(sj, "danger");
  if (rj && ccord_json_type_of(rj) == CCORD_JSON_BOOL) { st.has_danger = true; st.danger = ccord_json_bool(rj, false); }

  ccord_error err = {0};
  if (ccord_contrib_set_menu_state(m->contrib, p->m.id, item_id, tid, &st, &err) != CCORD_OK) {
    pl_reply_err(m, p, id, "invalid_argument", err.message);
    return;
  }
  pl_reply_ok(m, p, id, nullptr);
}

static void req_menu_clear_state(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0;
  const char *item = pl_str_param(params, "id", &il);
  if (!item || !pl_valid_menu_id(item, il)) {
    pl_reply_err(m, p, id, "invalid_argument", "menu id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  char item_id[40];
  snprintf(item_id, sizeof item_id, "%.*s", (int)il, item);

  bool clear_all = true;
  ccord_id tid = CCORD_ID_NONE;
  const ccord_json *tj = params ? ccord_json_get(params, "target_id") : nullptr;
  if (tj && ccord_json_type_of(tj) != CCORD_JSON_NULL) {
    size_t tl = 0;
    const char *ts = ccord_json_string(tj, &tl);
    if (!ts || !parse_menu_state_target_id(ts, tl, &tid)) {
      pl_reply_err(m, p, id, "invalid_argument", "invalid target_id");
      return;
    }
    clear_all = false;
  }
  (void)ccord_contrib_clear_menu_state(m->contrib, p->m.id, item_id, tid, clear_all);
  pl_reply_ok(m, p, id, nullptr);
}

static void req_menu_remove(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0;
  const char *item = pl_str_param(params, "id", &il);
  if (!item || !pl_valid_menu_id(item, il)) {
    pl_reply_err(m, p, id, "invalid_argument", "menu id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  char item_id[40];
  snprintf(item_id, sizeof item_id, "%.*s", (int)il, item);
  if (ccord_contrib_remove_menu(m->contrib, p->m.id, item_id) && p->menu_count > 0) p->menu_count--;
  pl_reply_ok(m, p, id, nullptr);
}

bool pl_parse_panel_slot(const char *s, ccord_panel_slot *out)
{
  if (!s) return false;
  static const struct { const char *name; ccord_panel_slot slot; } k_slots[] = {
    {"top", CCORD_PANEL_TOP}, {"subheader", CCORD_PANEL_SUBHEADER}, {"bottom", CCORD_PANEL_BOTTOM},
    {"right_sidebar", CCORD_PANEL_RIGHT_SIDEBAR}, {"left_sidebar", CCORD_PANEL_LEFT_SIDEBAR},
    {"channel_top", CCORD_PANEL_CHANNEL_TOP}, {"channel_bottom", CCORD_PANEL_CHANNEL_BOTTOM},
    {"rail_top", CCORD_PANEL_RAIL_TOP}, {"rail_bottom", CCORD_PANEL_RAIL_BOTTOM},
    {"header_right", CCORD_PANEL_HEADER_RIGHT},
    {"composer_top", CCORD_PANEL_COMPOSER_TOP}, {"composer_left", CCORD_PANEL_COMPOSER_LEFT},
    {"composer_right", CCORD_PANEL_COMPOSER_RIGHT}, {"voice_bar", CCORD_PANEL_VOICE_BAR},
    {"stage_overlay", CCORD_PANEL_STAGE_OVERLAY}, {"full_view", CCORD_PANEL_FULL_VIEW},
    {"modal", CCORD_PANEL_MODAL}, {"popover", CCORD_PANEL_POPOVER}, {"floating", CCORD_PANEL_FLOATING},
  };
  for (size_t i = 0; i < sizeof k_slots / sizeof k_slots[0]; i++) {
    if (strcmp(s, k_slots[i].name) == 0) { *out = k_slots[i].slot; return true; }
  }
  return false;
}

static void req_panel_mount(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0, sl = 0, tl = 0, icl = 0;
  const char *pid = pl_str_param(params, "id", &il);
  const char *slot_s = pl_str_param(params, "slot", &sl);
  const ccord_json *hj = params ? ccord_json_get(params, "height") : nullptr;
  const ccord_json *root = params ? ccord_json_get(params, "root") : nullptr;
  if (!pid || !pl_valid_menu_id(pid, il)) {
    pl_reply_err(m, p, id, "invalid_argument", "panel id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  ccord_panel_slot slot = CCORD_PANEL_TOP;
  if (slot_s && !pl_parse_panel_slot(slot_s, &slot)) {
    pl_reply_err(m, p, id, "invalid_argument", "unknown panel slot");
    return;
  }
  double h = hj ? ccord_json_number(hj, 34.0) : 34.0;
  if (!(h >= 12.0 && h <= 2048.0)) {
    pl_reply_err(m, p, id, "invalid_argument", "panel height must be between 12 and 2048");
    return;
  }
  if (!root || ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
    pl_reply_err(m, p, id, "invalid_argument", "panel root must be an object");
    return;
  }
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  pl_jw_copy_json(&w, root);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    pl_reply_err(m, p, id, "invalid_argument", "panel root could not be encoded");
    return;
  }
  char panel_id[40];
  snprintf(panel_id, sizeof panel_id, "%.*s", (int)il, pid);
  const ccord_json *wj = params ? ccord_json_get(params, "width") : nullptr;
  const ccord_json *xj = params ? ccord_json_get(params, "x") : nullptr;
  const ccord_json *yj = params ? ccord_json_get(params, "y") : nullptr;
  const ccord_json *vj = params ? ccord_json_get(params, "visible") : nullptr;
  ccord_panel_spec spec = {
    .slot = slot,
    .height = (float)h,
    .width = wj ? (float)ccord_json_number(wj, 0.0) : 0.0f,
    .x = xj ? (float)ccord_json_number(xj, 0.0) : 0.0f,
    .y = yj ? (float)ccord_json_number(yj, 0.0) : 0.0f,
    .title = pl_str_param(params, "title", &tl),
    .icon = pl_str_param(params, "icon", &icl),
    .visible = vj ? ccord_json_bool(vj, true) : true,
    .root_json = ccord_buf_cstr(&b),
  };
  ccord_error err = {0};
  ccord_status st = ccord_contrib_set_panel_ex(m->contrib, p->m.id, panel_id, &spec, &err);
  ccord_buf_free(&b);
  if (st != CCORD_OK) {
    pl_reply_err(m, p, id, "invalid_argument", err.message);
    return;
  }
  pl_reply_ok(m, p, id, nullptr);
}

static void req_panel_update(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0, sl = 0, tl = 0, icl = 0;
  const char *pid = pl_str_param(params, "id", &il);
  if (!pid || !pl_valid_menu_id(pid, il)) {
    pl_reply_err(m, p, id, "invalid_argument", "panel id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  char panel_id[40];
  snprintf(panel_id, sizeof panel_id, "%.*s", (int)il, pid);
  const ccord_panel_contribution *cur = ccord_contrib_find_panel(m->contrib, p->m.id, panel_id);
  if (!cur) {
    pl_reply_err(m, p, id, "not_found", "panel is not mounted");
    return;
  }
  ccord_panel_slot slot = cur->slot;
  const char *slot_s = pl_str_param(params, "slot", &sl);
  if (slot_s && !pl_parse_panel_slot(slot_s, &slot)) {
    pl_reply_err(m, p, id, "invalid_argument", "unknown panel slot");
    return;
  }
  float height = cur->height;
  const ccord_json *hj = params ? ccord_json_get(params, "height") : nullptr;
  if (hj && ccord_json_type_of(hj) == CCORD_JSON_NUMBER) {
    double h = ccord_json_number(hj, (double)height);
    if (!(h >= 12.0 && h <= 2048.0)) {
      pl_reply_err(m, p, id, "invalid_argument", "panel height must be between 12 and 2048");
      return;
    }
    height = (float)h;
  }
  const ccord_json *root = params ? ccord_json_get(params, "root") : nullptr;
  ccord_buf b;
  ccord_buf_init(&b, 0);
  const char *root_str = cur->root_json;
  if (root) {
    if (ccord_json_type_of(root) != CCORD_JSON_OBJECT) {
      ccord_buf_free(&b);
      pl_reply_err(m, p, id, "invalid_argument", "panel root must be an object");
      return;
    }
    ccord_jw w;
    ccord_jw_init(&w, &b);
    pl_jw_copy_json(&w, root);
    if (!ccord_jw_ok(&w)) {
      ccord_buf_free(&b);
      pl_reply_err(m, p, id, "invalid_argument", "panel root could not be encoded");
      return;
    }
    root_str = ccord_buf_cstr(&b);
  }
  const ccord_json *wj = params ? ccord_json_get(params, "width") : nullptr;
  const ccord_json *xj = params ? ccord_json_get(params, "x") : nullptr;
  const ccord_json *yj = params ? ccord_json_get(params, "y") : nullptr;
  const ccord_json *vj = params ? ccord_json_get(params, "visible") : nullptr;
  const char *ntitle = pl_str_param(params, "title", &tl);
  const char *nicon = pl_str_param(params, "icon", &icl);
  ccord_panel_spec spec = {
    .slot = slot,
    .height = height,
    .width = wj ? (float)ccord_json_number(wj, (double)cur->width) : cur->width,
    .x = xj ? (float)ccord_json_number(xj, (double)cur->x) : cur->x,
    .y = yj ? (float)ccord_json_number(yj, (double)cur->y) : cur->y,
    .title = ntitle ? ntitle : cur->title,
    .icon = nicon ? nicon : cur->icon,
    .visible = vj ? ccord_json_bool(vj, cur->visible) : cur->visible,
    .root_json = root_str,
  };
  ccord_error err = {0};
  ccord_status st = ccord_contrib_set_panel_ex(m->contrib, p->m.id, panel_id, &spec, &err);
  ccord_buf_free(&b);
  if (st != CCORD_OK) {
    pl_reply_err(m, p, id, "invalid_argument", err.message);
    return;
  }
  pl_reply_ok(m, p, id, nullptr);
}

static void req_panel_unmount(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t il = 0;
  const char *pid = pl_str_param(params, "id", &il);
  if (!pid || !pl_valid_menu_id(pid, il)) {
    pl_reply_err(m, p, id, "invalid_argument", "panel id must match ^[a-z0-9][a-z0-9_-]{0,31}$");
    return;
  }
  char panel_id[40];
  snprintf(panel_id, sizeof panel_id, "%.*s", (int)il, pid);
  (void)ccord_contrib_remove_panel(m->contrib, p->m.id, panel_id);
  pl_reply_ok(m, p, id, nullptr);
}

static void req_clipboard(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t len = 0;
  const char *text = pl_str_param(params, "text", &len);
  if (!text || len > MAX_CLIPBOARD_BYTES || !ccord_utf8_valid_text(text, len)) {
    { pl_reply_err(m, p, id, "invalid_argument", "clipboard text must be valid UTF-8 text up to 65536 bytes"); return; }
  }
  if (!m->clipboard_write) { pl_reply_err(m, p, id, "unsupported", "clipboard is not available"); return; }
  char *copy = ccord_strndup(text, len);
  if (!copy) { pl_reply_err(m, p, id, "internal", "out of memory"); return; }
  m->clipboard_write(m->clipboard_ud, copy);
  ccord_free(copy);
  pl_reply_ok(m, p, id, nullptr);
}

static bool is_reserved_command(const char *name)
{
  static const char *const k_reserved[] = {
    "tts", "me", "shrug", "tableflip", "unflip", "nick", "spoiler", "giphy", "tenor", "msg", "thread",
  };
  for (size_t i = 0; i < sizeof k_reserved / sizeof k_reserved[0]; i++) {
    if (strcmp(name, k_reserved[i]) == 0) return true;
  }
  return false;
}

static void req_commands_register(ccord_plugins *m, plugin *p, uint32_t id, const ccord_json *params)
{
  size_t nl = 0, dl = 0;
  const char *name = pl_str_param(params, "name", &nl);
  const char *desc = pl_str_param(params, "description", &dl);
  if (!name || !pl_valid_command_name(name, nl)) { pl_reply_err(m, p, id, "invalid_argument", "command name must match ^[a-z][a-z0-9_-]{0,31}$"); return; }
  if (!desc || !pl_valid_plain_text(desc, dl, 100)) { pl_reply_err(m, p, id, "invalid_argument", "description must be 1-100 bytes of text"); return; }
  char name_s[40], desc_s[104];
  snprintf(name_s, sizeof name_s, "%.*s", (int)nl, name);
  snprintf(desc_s, sizeof desc_s, "%.*s", (int)dl, desc);
  if (is_reserved_command(name_s)) {
    pl_reply_err(m, p, id, "invalid_argument", "command name is reserved by C-Cord");
    return;
  }
  ccord_buf schema_b; ccord_buf_init(&schema_b, 0);
  const ccord_json *aj = params ? ccord_json_get(params, "args") : nullptr;
  if (aj) { ccord_jw jw; ccord_jw_init(&jw, &schema_b); pl_jw_copy_json(&jw, aj); }
  bool has_ac = params && ccord_json_bool(ccord_json_get(params, "has_autocomplete"), false);
  ccord_error err = {0};
  ccord_status st = ccord_contrib_add_command_ex(m->contrib, p->m.id, name_s, desc_s,
                                                 schema_b.len ? ccord_buf_cstr(&schema_b) : "", has_ac, &err);
  ccord_buf_free(&schema_b);
  if (st != CCORD_OK) {
    { pl_reply_err(m, p, id, "invalid_argument", err.message); return; }
  }
  pl_reply_ok(m, p, id, nullptr);
}

ccord_id pl_parse_id_param(const ccord_json *params, const char *key)
{
  size_t n = 0;
  const char *s = pl_str_param(params, key, &n);
  if (!s || !n) return 0;
  if (strcmp(s, "dm") == 0) return CCORD_DM_GUILD_ID;
  return (ccord_id)strtoull(s, nullptr, 10);
}

const char *pl_channel_kind_str(const ccord_store *st, const ccord_channel *c)
{
  if (!c) return "text";
  if (c->guild_id == CCORD_DM_GUILD_ID) {
    return (c->discord_type == CCORD_DISCORD_CHANNEL_GROUP_DM || c->recipient_count > 1u) ? "group_dm" : "dm";
  }
  if (st && ccord_store_channel_is_forum(st, c->id)) return "forum";
  if (ccord_discord_type_is_thread(c->discord_type)) return "thread";
  if (c->kind == CCORD_CHANNEL_CATEGORY) return "category";
  if (c->kind == CCORD_CHANNEL_STAGE || c->discord_type == CCORD_DISCORD_CHANNEL_GUILD_STAGE_VOICE) return "stage";
  if (c->kind == CCORD_CHANNEL_VOICE) return "voice";
  return "text";
}

void pl_channel_display_name(const ccord_store *st, const ccord_channel *c, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!c) return;
  if (c->name && c->name[0]) {
    snprintf(out, cap, "%s", c->name);
    return;
  }
  if (c->guild_id == CCORD_DM_GUILD_ID && st) {
    if (c->recipient_count > 1u) {
      size_t pos = 0;
      for (uint16_t i = 0; i < c->recipient_count && i < CCORD_MAX_DM_RECIPIENTS; i++) {
        const ccord_user *u = ccord_store_user(st, c->recipient_ids[i]);
        const char *un = (u && u->name && u->name[0]) ? u->name : (u && u->handle && u->handle[0] ? u->handle : nullptr);
        if (!un) continue;
        int n = snprintf(out + pos, cap - pos, "%s%s", pos ? ", " : "", un);
        if (n < 0 || (size_t)n >= cap - pos) break;
        pos += (size_t)n;
      }
      if (out[0]) return;
    }
    ccord_id uid = c->user_id ? c->user_id : (c->recipient_count > 0 ? c->recipient_ids[0] : CCORD_ID_NONE);
    const ccord_user *u = uid ? ccord_store_user(st, uid) : nullptr;
    if (u && u->name && u->name[0]) { snprintf(out, cap, "%s", u->name); return; }
    if (u && u->handle && u->handle[0]) { snprintf(out, cap, "%s", u->handle); return; }
    snprintf(out, cap, "Direct Message");
    return;
  }
  snprintf(out, cap, "Channel");
}

static void write_channel_obj(ccord_jw *w, const ccord_store *st, const ccord_channel *c, int64_t now_unix_ms)
{
  char dname[192];
  pl_channel_display_name(st, c, dname, sizeof dname);
  bool muted = st ? ccord_store_channel_is_muted(st, c, now_unix_ms) : false;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "id"); ccord_jw_id(w, c->id);
  ccord_jw_key(w, "guild_id"); ccord_jw_id(w, c->guild_id);
  if (c->parent_id != CCORD_ID_NONE) {
    ccord_jw_key(w, "parent_id"); ccord_jw_id(w, c->parent_id);
  }
  ccord_jw_key(w, "name"); ccord_jw_cstr(w, c->name ? c->name : "");
  ccord_jw_key(w, "display_name"); ccord_jw_cstr(w, dname);
  ccord_jw_key(w, "topic"); ccord_jw_cstr(w, c->topic ? c->topic : "");
  ccord_jw_key(w, "kind"); ccord_jw_cstr(w, pl_channel_kind_str(st, c));
  ccord_jw_key(w, "is_dm"); ccord_jw_bool(w, c->guild_id == CCORD_DM_GUILD_ID);
  ccord_jw_key(w, "unread_count"); ccord_jw_int(w, (int64_t)c->unread_count);
  ccord_jw_key(w, "mention_count"); ccord_jw_int(w, (int64_t)c->mention_count);
  ccord_jw_key(w, "muted"); ccord_jw_bool(w, muted);
  ccord_jw_end_object(w);
}

static void req_store(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params)
{
  if (!m->store) { pl_reply_err(m, p, id, "unsupported", "store unavailable"); return; }
  int64_t now_unix = ccord_clock_unix_ms();
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  if (strcmp(method, "store.self") == 0) {
    ccord_id sid = ccord_store_self_id(m->store);
    const ccord_user *u = sid ? ccord_store_user(m->store, sid) : nullptr;
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_id(&w, sid);
    ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, u && u->name ? u->name : "");
    ccord_jw_end_object(&w);
  } else if (strcmp(method, "store.selected") == 0) {
    ccord_id sg = ccord_store_selected_guild(m->store);
    ccord_id sc = ccord_store_selected_channel(m->store);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, sg);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, sc);
    ccord_jw_key(&w, "view"); ccord_jw_cstr(&w, ccord_contrib_ui_view(m->contrib));
    ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, sg == CCORD_DM_GUILD_ID);
    ccord_jw_end_object(&w);
  } else if (strcmp(method, "store.guilds") == 0) {
    ccord_jw_begin_array(&w);
    size_t n = ccord_store_guild_count(m->store);
    for (size_t i = 0; i < n; i++) {
      const ccord_guild *g = ccord_store_guild_at(m->store, i);
      if (!g) continue;
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_id(&w, g->id);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, g->name ? g->name : "");
      ccord_jw_key(&w, "muted"); ccord_jw_bool(&w, ccord_store_guild_muted(m->store, g->id, now_unix));
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
  } else if (strcmp(method, "store.guild") == 0) {
    ccord_id gid = pl_parse_id_param(params, "guild_id");
    if (gid == CCORD_DM_GUILD_ID) {
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_id(&w, CCORD_DM_GUILD_ID);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, CCORD_DM_GUILD_NAME);
      ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, true);
      ccord_jw_key(&w, "muted"); ccord_jw_bool(&w, false);
      ccord_jw_end_object(&w);
    } else {
      const ccord_guild *g = ccord_store_guild(m->store, gid);
      if (!g) { ccord_buf_free(&b); pl_reply_err(m, p, id, "not_found", "guild not found"); return; }
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "id"); ccord_jw_id(&w, g->id);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, g->name ? g->name : "");
      ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, false);
      ccord_jw_key(&w, "muted"); ccord_jw_bool(&w, ccord_store_guild_muted(m->store, g->id, now_unix));
      ccord_jw_end_object(&w);
    }
  } else if (strcmp(method, "store.channels") == 0) {
    ccord_id gid = pl_parse_id_param(params, "guild_id");
    ccord_jw_begin_array(&w);
    size_t n = ccord_store_channel_count(m->store, gid);
    for (size_t i = 0; i < n; i++) {
      const ccord_channel *c = ccord_store_channel_at(m->store, gid, i);
      if (!c) continue;
      write_channel_obj(&w, m->store, c, now_unix);
    }
    ccord_jw_end_array(&w);
  } else if (strcmp(method, "store.channel") == 0) {
    const ccord_channel *c = ccord_store_channel(m->store, pl_parse_id_param(params, "channel_id"));
    if (!c) { ccord_buf_free(&b); pl_reply_err(m, p, id, "not_found", "channel not found"); return; }
    write_channel_obj(&w, m->store, c, now_unix);
  } else if (strcmp(method, "store.user") == 0) {
    const ccord_user *u = ccord_store_user(m->store, pl_parse_id_param(params, "user_id"));
    if (!u) { ccord_buf_free(&b); pl_reply_err(m, p, id, "not_found", "user not found"); return; }
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "id"); ccord_jw_id(&w, u->id);
    ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, u->name ? u->name : "");
    ccord_jw_end_object(&w);
  } else if (strcmp(method, "store.messages") == 0) {
    ccord_id cid = pl_parse_id_param(params, "channel_id");
    int64_t lim = 50;
    const ccord_json *lj = params ? ccord_json_get(params, "limit") : nullptr;
    if (lj) (void)ccord_json_int64(lj, &lim);
    if (lim <= 0) lim = 50;
    size_t tot = ccord_store_message_count(m->store, cid);
    size_t start = tot > (size_t)lim ? tot - (size_t)lim : 0;
    ccord_jw_begin_array(&w);
    for (size_t i = start; i < tot; i++) {
      const ccord_message *msg = ccord_store_message_at(m->store, cid, i);
      if (!msg) continue;
      plugins_ext_write_message_obj(&w, m->store, msg);
    }
    ccord_jw_end_array(&w);
  } else if (strcmp(method, "store.message") == 0) {
    const ccord_message *msg = ccord_store_find_message(m->store, pl_parse_id_param(params, "channel_id"), pl_parse_id_param(params, "message_id"));
    if (!msg) { ccord_buf_free(&b); pl_reply_err(m, p, id, "not_found", "message not found"); return; }
    plugins_ext_write_message_obj(&w, m->store, msg);
  } else if (strcmp(method, "store.members") == 0) {
    ccord_id gid = pl_parse_id_param(params, "guild_id");
    size_t n = ccord_store_member_count(m->store, gid);
    ccord_jw_begin_array(&w);
    for (size_t i = 0; i < n; i++) {
      const ccord_member *mb = ccord_store_member_at(m->store, gid, i);
      if (!mb) continue;
      const ccord_user *u = ccord_store_user(m->store, mb->user_id);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "user_id"); ccord_jw_id(&w, mb->user_id);
      ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, u && u->name ? u->name : "");
      ccord_jw_key(&w, "nick"); ccord_jw_cstr(&w, mb->nick);
      ccord_jw_end_object(&w);
    }
    ccord_jw_end_array(&w);
  } else {
    ccord_buf_free(&b);
    ccord_ipc_msg sub = {.id = id, .method = method, .params = params};
    if (!plugins_ext_handle_request(m, p, &sub)) pl_reply_err(m, p, id, "unsupported", "unknown store method");
    return;
  }
  if (ccord_jw_ok(&w)) pl_reply_ok(m, p, id, ccord_buf_cstr(&b));
  else pl_reply_err(m, p, id, "internal", "store encode failed");
  ccord_buf_free(&b);
}


static bool path_same_or_under(const char *path, const char *root)
{
  if (!path || !root || !root[0]) return false;
  size_t rl = strlen(root);
  while (rl > 1 && (root[rl - 1] == '/' || root[rl - 1] == '\\')) rl--;
#if CCORD_OS_WINDOWS
  if (_strnicmp(path, root, rl) != 0) return false;
#else
  if (strncmp(path, root, rl) != 0) return false;
#endif
  return path[rl] == '\0' || path[rl] == '/' || path[rl] == '\\';
}

// Resolves `raw_path` even if trailing components do not yet exist (by resolving the deepest existing ancestor).
static bool resolve_canonical_target(const char *raw_path, char *out, size_t cap)
{
  if (!raw_path || !raw_path[0] || !out || cap == 0) return false;
  if (pl_resolve_real_path(raw_path, out, cap)) return true;

  char cur[CCORD_PATH_MAX];
  if (snprintf(cur, sizeof cur, "%s", raw_path) >= (int)sizeof cur) return false;
  size_t clen = strlen(cur);
  while (clen > 1 && (cur[clen - 1] == '/' || cur[clen - 1] == '\\')) cur[--clen] = '\0';

  const char *tail_start = cur + clen;
  for (;;) {
    char *sep = nullptr;
    for (char *q = cur + (size_t)(tail_start - cur); q > cur; q--) {
      if (q[-1] == '/' || q[-1] == '\\') { sep = q - 1; break; }
    }
    if (!sep) {
      // Relative path with no existing prefix: resolve against CWD.
      char cwd[CCORD_PATH_MAX];
      if (!pl_resolve_real_path(".", cwd, sizeof cwd)) return false;
      if (strstr(raw_path, "..")) return false;
      return snprintf(out, cap, "%s/%s", cwd, raw_path) < (int)cap;
    }
    char saved = *sep;
    if (sep == cur) {
      // Root slash
      char root_str[2] = {saved, '\0'};
      const char *rest = sep + 1;
      if (strstr(rest, "..")) return false;
      char rbuf[CCORD_PATH_MAX];
      if (!pl_resolve_real_path(root_str, rbuf, sizeof rbuf)) return false;
      return snprintf(out, cap, "%s%s", rbuf, rest) < (int)cap;
    }
    *sep = '\0';
    char rbuf[CCORD_PATH_MAX];
    if (pl_resolve_real_path(cur, rbuf, sizeof rbuf)) {
      const char *rest = sep + 1;
      // Reject any ".." segment in the non-existent suffix.
      const char *p = rest;
      while (*p) {
        const char *ns = p;
        while (*ns && *ns != '/' && *ns != '\\') ns++;
        size_t slen = (size_t)(ns - p);
        if (slen == 2 && p[0] == '.' && p[1] == '.') return false;
        p = *ns ? ns + 1 : ns;
      }
      int n = snprintf(out, cap, "%s/%s", rbuf, rest);
      return n > 0 && (size_t)n < cap;
    }
    *sep = saved;
    tail_start = sep;
  }
}

bool pl_fs_path_is_protected(const ccord_plugins *m, const char *raw_path)
{
  char target[CCORD_PATH_MAX];
  if (!resolve_canonical_target(raw_path, target, sizeof target)) return true;

  char rdir[CCORD_PATH_MAX];
  if (m && m->has_data_dir && m->data_dir && resolve_canonical_target(m->data_dir, rdir, sizeof rdir)) {
    if (path_same_or_under(target, rdir)) return true;
  }
  const char *gdata = ccord_paths_data_dir();
  if (gdata && gdata[0] && resolve_canonical_target(gdata, rdir, sizeof rdir)) {
    if (path_same_or_under(target, rdir)) return true;
  }
  const char *exe = ccord_paths_exe_path();
  if (exe && exe[0]) {
    if (path_same_or_under(target, exe)) return true;
    if (resolve_canonical_target(exe, rdir, sizeof rdir) && path_same_or_under(target, rdir)) return true;
  }
#if defined(__linux__) && !CCORD_OS_WINDOWS
  if (pl_resolve_real_path("/proc/self/exe", rdir, sizeof rdir) && path_same_or_under(target, rdir)) {
    return true;
  }
#endif
  if (m) {
    for (size_t i = 0; i < m->nsearch; i++) {
      if (m->search[i] && resolve_canonical_target(m->search[i], rdir, sizeof rdir)) {
        if (path_same_or_under(target, rdir)) return true;
      }
    }
  }
  // Protect C-Cord configuration / token directories, credential stores, and shell/autostart persistence targets.
  char cfg_cand[16][CCORD_PATH_MAX];
  size_t nc = 0;
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && xdg[0]) {
    int n = snprintf(cfg_cand[nc], sizeof cfg_cand[0], "%s/ccord", xdg);
    if (n > 0 && (size_t)n < sizeof cfg_cand[0]) nc++;
    if (nc < 16u) {
      n = snprintf(cfg_cand[nc], sizeof cfg_cand[0], "%s/autostart", xdg);
      if (n > 0 && (size_t)n < sizeof cfg_cand[0]) nc++;
    }
  }
  const char *home = getenv("HOME");
  if (home && home[0]) {
    static const char *const k_home_protected[] = {
      ".config/ccord",
      "Library/Application Support/ccord",
      ".local/bin/C-Cord.AppImage",
      ".config/autostart",
      ".bashrc",
      ".profile",
      ".zshrc",
      ".bash_profile",
      ".ssh",
      ".gnupg",
    };
    for (size_t i = 0; i < sizeof k_home_protected / sizeof k_home_protected[0] && nc < 16u; i++) {
      int n = snprintf(cfg_cand[nc], sizeof cfg_cand[0], "%s/%s", home, k_home_protected[i]);
      if (n > 0 && (size_t)n < sizeof cfg_cand[0]) nc++;
    }
  }
  const char *appdata = getenv("APPDATA");
  if (appdata && appdata[0] && nc < 16u) {
    int n = snprintf(cfg_cand[nc], sizeof cfg_cand[0], "%s/ccord", appdata);
    if (n > 0 && (size_t)n < sizeof cfg_cand[0]) nc++;
  }
  for (size_t i = 0; i < nc; i++) {
    if (resolve_canonical_target(cfg_cand[i], rdir, sizeof rdir) && path_same_or_under(target, rdir)) return true;
  }
  return false;
}

static void req_fs(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params)
{
  bool is_write = (strcmp(method, "fs.write") == 0 || strcmp(method, "fs.mkdir") == 0 || strcmp(method, "fs.remove") == 0);
  ccord_perm need = is_write ? CCORD_PERM_FS_WRITE : CCORD_PERM_FS_READ;
  if (!pl_has_perm(p, need)) {
    pl_fail_plugin(m, p, is_write ? "undeclared restricted API called: fs.write" : "undeclared restricted API called: fs.read");
    return;
  }
  size_t plen = 0;
  const char *path = pl_str_param(params, "path", &plen);
  if (!path || plen == 0 || plen >= CCORD_PATH_MAX) {
    pl_reply_err(m, p, id, "invalid_argument", "fs: invalid path");
    return;
  }
  if (pl_fs_path_is_protected(m, path)) {
    pl_reply_err(m, p, id, "permission_denied", "access to C-Cord internal config/plugin paths is forbidden");
    return;
  }

  if (strcmp(method, "fs.exists") == 0) {
    pl_reply_ok(m, p, id, ccord_path_exists(path) ? "{\"exists\":true}" : "{\"exists\":false}");
    return;
  }
  if (strcmp(method, "fs.list") == 0) {
    ccord_dir *d = ccord_dir_open(path);
    if (!d) { pl_reply_err(m, p, id, "io", strerror(errno)); return; }
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_array(&w);
    char name[256];
    size_t count = 0;
    while (ccord_dir_next(d, name, sizeof name, nullptr) && count < 4096u) {
      ccord_jw_cstr(&w, name);
      count++;
    }
    ccord_dir_close(d);
    ccord_jw_end_array(&w);
    if (ccord_jw_ok(&w)) pl_reply_ok(m, p, id, ccord_buf_cstr(&b));
    else pl_reply_err(m, p, id, "internal", "fs.list encode failed");
    ccord_buf_free(&b);
    return;
  }
  if (strcmp(method, "fs.mkdir") == 0) {
    ccord_error err = {0};
    if (ccord_mkdir_p(path, 0755, &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "io", err.message[0] ? err.message : "mkdir failed");
    } else {
      pl_reply_ok(m, p, id, nullptr);
    }
    return;
  }
  if (strcmp(method, "fs.remove") == 0) {
#if CCORD_OS_WINDOWS
    if (remove(path) != 0) pl_reply_err(m, p, id, "io", strerror(errno));
    else pl_reply_ok(m, p, id, nullptr);
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
      pl_reply_err(m, p, id, "io", strerror(errno));
      return;
    }
    if (S_ISLNK(st.st_mode)) {
      pl_reply_err(m, p, id, "permission_denied", "refusing to remove symbolic link");
      return;
    }
    int rrc = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    if (rrc != 0) pl_reply_err(m, p, id, "io", strerror(errno));
    else pl_reply_ok(m, p, id, nullptr);
#endif
    return;
  }
  if (strcmp(method, "fs.write") == 0) {
    size_t b64_len = 0;
    const char *b64 = pl_str_param(params, "b64", &b64_len);
    bool append = ccord_json_bool(ccord_json_get(params, "append"), false);
    if (!b64) { pl_reply_err(m, p, id, "invalid_argument", "fs.write: missing data"); return; }
    size_t raw_cap = (b64_len / 4u) * 3u;
    if (raw_cap > MAX_FS_CHUNK_BYTES + 3u) { pl_reply_err(m, p, id, "invalid_argument", "fs.write: chunk too large"); return; }
    uint8_t *raw = raw_cap ? ccord_malloc(raw_cap) : nullptr;
    if (raw_cap && !raw) { pl_reply_err(m, p, id, "internal", "out of memory"); return; }
    size_t raw_len = 0;
    if (b64_len > 0 && !pl_fs_b64_decode((const unsigned char *)b64, b64_len, raw, &raw_len)) {
      ccord_free(raw);
      pl_reply_err(m, p, id, "invalid_argument", "fs.write: invalid base64");
      return;
    }
#if CCORD_OS_WINDOWS
    FILE *f = fopen(path, append ? "ab" : "wb");
    if (!f) {
      ccord_free(raw);
      pl_reply_err(m, p, id, "io", strerror(errno));
      return;
    }
    size_t wr = raw_len ? fwrite(raw, 1, raw_len, f) : 0;
    fclose(f);
#else
    int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (append ? O_APPEND : 0);
    int fd = open(path, flags, 0644);
    if (fd < 0) {
      ccord_free(raw);
      pl_reply_err(m, p, id, "io", strerror(errno));
      return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      close(fd);
      ccord_free(raw);
      pl_reply_err(m, p, id, "io", "not a regular file");
      return;
    }
    if (!append && ftruncate(fd, 0) != 0) {
      int e = errno;
      close(fd);
      ccord_free(raw);
      pl_reply_err(m, p, id, "io", strerror(e));
      return;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    size_t wr = 0;
    while (wr < raw_len) {
      ssize_t wn = write(fd, raw + wr, raw_len - wr);
      if (wn < 0) {
        if (errno == EINTR) continue;
        break;
      }
      wr += (size_t)wn;
    }
    close(fd);
#endif
    ccord_free(raw);
    if (wr != raw_len) pl_reply_err(m, p, id, "io", "short write");
    else pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "fs.read") == 0) {
    int64_t off_i = 0, max_i = (int64_t)MAX_FS_CHUNK_BYTES;
    const ccord_json *oj = ccord_json_get(params, "offset");
    const ccord_json *mj = ccord_json_get(params, "max_bytes");
    if (!mj) mj = ccord_json_get(params, "length");
    if (oj) (void)ccord_json_int64(oj, &off_i);
    if (mj) (void)ccord_json_int64(mj, &max_i);
    if (off_i < 0 || (uint64_t)off_i > MAX_FS_READ_BYTES) {
      pl_reply_err(m, p, id, "invalid_argument", "file exceeds 16 MiB limit");
      return;
    }
    if (max_i <= 0 || (size_t)max_i > MAX_FS_CHUNK_BYTES) max_i = (int64_t)MAX_FS_CHUNK_BYTES;
    FILE *f = nullptr;
    uint64_t fsz = 0;
#if CCORD_OS_WINDOWS
    f = fopen(path, "rb");
    if (!f) { pl_reply_err(m, p, id, "io", strerror(errno)); return; }
    struct __stat64 st;
    if (_fstat64(_fileno(f), &st) != 0 || (st.st_mode & _S_IFREG) == 0 || st.st_size < 0) {
      fclose(f);
      pl_reply_err(m, p, id, "io", "not a regular file");
      return;
    }
    fsz = (uint64_t)st.st_size;
#else
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { pl_reply_err(m, p, id, "io", strerror(errno)); return; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
      close(fd);
      pl_reply_err(m, p, id, "io", "not a regular file");
      return;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    f = fdopen(fd, "rb");
    if (!f) { close(fd); pl_reply_err(m, p, id, "io", strerror(errno)); return; }
    fsz = (uint64_t)st.st_size;
#endif
    if (fsz > MAX_FS_READ_BYTES) {
      fclose(f);
      pl_reply_err(m, p, id, "invalid_argument", "file exceeds 16 MiB limit");
      return;
    }
    if (off_i > 0 && fseek(f, (long)off_i, SEEK_SET) != 0) {
      fclose(f);
      pl_reply_err(m, p, id, "io", "seek failed");
      return;
    }
    size_t want = (size_t)max_i;
    uint8_t *buf = ccord_malloc(want + 1u);
    if (!buf) { fclose(f); pl_reply_err(m, p, id, "internal", "out of memory"); return; }
    size_t rd = fread(buf, 1, want, f);
    bool eof = (rd < want) || feof(f) != 0 || (fsz > 0 && (uint64_t)off_i + rd >= fsz);
    fclose(f);
    ccord_buf b64; ccord_buf_init(&b64, (rd / 3u + 2u) * 4u);
    pl_fs_b64_encode(buf, rd, &b64);
    ccord_free(buf);
    ccord_buf out; ccord_buf_init(&out, b64.len + 64u);
    ccord_jw w; ccord_jw_init(&w, &out);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
    ccord_jw_key(&w, "eof"); ccord_jw_bool(&w, eof);
    ccord_jw_end_object(&w);
    ccord_buf_free(&b64);
    if (ccord_jw_ok(&w)) pl_reply_ok(m, p, id, ccord_buf_cstr(&out));
    else pl_reply_err(m, p, id, "internal", "fs.read encode failed");
    ccord_buf_free(&out);
    return;
  }
  pl_reply_err(m, p, id, "unsupported", "unknown fs method");
}

static void handle_request(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  uint32_t id = msg->id;
  const char *method = msg->method;
  const ccord_json *params = msg->params;

  // Class 1 (Safe) APIs — no manifest permission required
  if (strcmp(method, "ui.contextMenu.add") == 0) { req_menu_add(m, p, id, params); return; }
  if (strcmp(method, "ui.contextMenu.remove") == 0) { req_menu_remove(m, p, id, params); return; }
  if (strcmp(method, "ui.contextMenu.set_state") == 0) { req_menu_set_state(m, p, id, params); return; }
  if (strcmp(method, "ui.contextMenu.clear_state") == 0) { req_menu_clear_state(m, p, id, params); return; }
  if (strcmp(method, "clipboard.write") == 0) { req_clipboard(m, p, id, params); return; }
  if (strcmp(method, "commands.register") == 0) { req_commands_register(m, p, id, params); return; }
  if (strcmp(method, "ui.toast.show") == 0) {
    size_t tl = 0, ll = 0;
    const char *txt = pl_str_param(params, "text", &tl);
    const char *lvl = pl_str_param(params, "level", &ll);
    uint8_t lv = (lvl && strcmp(lvl, "error") == 0) ? 2 : (lvl && strcmp(lvl, "warn") == 0) ? 1 : 0;
    if (txt && m->toast_show) m->toast_show(m->toast_ud, txt, lv);
    pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "storage.get") == 0 || strcmp(method, "storage.set") == 0 ||
      strcmp(method, "storage.delete") == 0 || strcmp(method, "storage.keys") == 0 ||
      strcmp(method, "storage.clear") == 0) {
    req_storage(m, p, id, method, params);
    return;
  }
  if (strcmp(method, "events.subscribe") == 0 || strcmp(method, "events.unsubscribe") == 0) {
    size_t nl = 0;
    const char *name = pl_str_param(params, "name", &nl);
    uint64_t bit = 0;
    ccord_perm perm = CCORD_PERM_EVENTS_NAVIGATION;
    char name_s[32];
    if (!name || nl >= sizeof name_s) { pl_reply_err(m, p, id, "invalid_argument", "unknown event name"); return; }
    snprintf(name_s, sizeof name_s, "%.*s", (int)nl, name);
    if (!event_bit_for(name_s, &bit, &perm)) { pl_reply_err(m, p, id, "invalid_argument", "unknown event name"); return; }
    if (strcmp(method, "events.subscribe") == 0) {
      if (ccord_perm_class_of(perm) != CCORD_PERM_CLASS_SAFE && !pl_has_perm(p, perm)) {
        pl_fail_plugin(m, p, "undeclared restricted API called");
        return;
      }
      p->subs |= bit;
    } else {
      p->subs &= ~bit;
    }
    pl_reply_ok(m, p, id, nullptr);
    return;
  }

  // Class 2 (Unsafe) APIs
  if (strncmp(method, "ui.panel.", 9) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_UI_PANEL)) { pl_fail_plugin(m, p, "undeclared restricted API called: ui.panel"); return; }
    if (strcmp(method, "ui.panel.mount") == 0) { req_panel_mount(m, p, id, params); return; }
    if (strcmp(method, "ui.panel.update") == 0) { req_panel_update(m, p, id, params); return; }
    if (strcmp(method, "ui.panel.unmount") == 0) { req_panel_unmount(m, p, id, params); return; }
    if (plugins_ext_handle_request(m, p, msg)) return;
    pl_reply_err(m, p, id, "unsupported", "unknown panel method");
    return;
  }
  if (strncmp(method, "store.", 6) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_STORE_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: store.read"); return; }
    req_store(m, p, id, method, params);
    return;
  }
  if (strcmp(method, "messages.send") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_MESSAGES_SEND)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.send"); return; }
    ccord_id cid = pl_parse_id_param(params, "channel_id");
    size_t cl = 0; const char *cnt = pl_str_param(params, "content", &cl);
    ccord_error err = {0};
    if (!m->session || !cid || !cnt || ccord_session_send_message(m->session, cid, cnt, cl, &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "send failed");
    } else pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "messages.edit") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return; }
    ccord_id cid = pl_parse_id_param(params, "channel_id"), mid = pl_parse_id_param(params, "message_id");
    size_t cl = 0; const char *cnt = pl_str_param(params, "content", &cl);
    ccord_error err = {0};
    if (!m->session || !cid || !mid || !cnt || ccord_session_edit_message(m->session, cid, mid, cnt, cl, &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "edit failed");
    } else pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "messages.delete") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return; }
    ccord_id cid = pl_parse_id_param(params, "channel_id"), mid = pl_parse_id_param(params, "message_id");
    ccord_error err = {0};
    if (!m->session || !cid || !mid || ccord_session_delete_message(m->session, cid, mid, &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "delete failed");
    } else pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "messages.typing") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return; }
    ccord_id cid = pl_parse_id_param(params, "channel_id");
    ccord_error err = {0};
    if (m->session && cid) (void)ccord_session_send_typing(m->session, cid, &err);
    pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "navigation.select_guild") == 0 || strcmp(method, "navigation.select_channel") == 0 ||
      strcmp(method, "navigation.select_view") == 0 || strcmp(method, "navigation.action") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_NAVIGATION_WRITE)) { pl_fail_plugin(m, p, "undeclared restricted API called: navigation.write"); return; }
    ccord_error err = {0};
    if (strcmp(method, "navigation.action") == 0) {
      size_t al = 0;
      const char *act = pl_str_param(params, "action", &al);
      if (act) (void)ccord_contrib_ui_action(m->contrib, act);
    } else if (strcmp(method, "navigation.select_view") == 0) {
      size_t vl = 0;
      const char *view = pl_str_param(params, "view", &vl);
      if (view && !ccord_contrib_ui_navigate(m->contrib, CCORD_ID_NONE, CCORD_ID_NONE, CCORD_ID_NONE, "", view)) {
        if (m->session && (strcmp(view, "friends") == 0 || strcmp(view, "requests") == 0)) {
          (void)ccord_session_select_guild(m->session, CCORD_DM_GUILD_ID, &err);
        }
      }
    } else if (strcmp(method, "navigation.select_guild") == 0) {
      ccord_id gid = pl_parse_id_param(params, "guild_id");
      if (!ccord_contrib_ui_navigate(m->contrib, gid, CCORD_ID_NONE, CCORD_ID_NONE, "", "guild") && m->session) {
        (void)ccord_session_select_guild(m->session, gid, &err);
      }
    } else {
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      ccord_id gid = pl_parse_id_param(params, "guild_id");
      ccord_id pid = pl_parse_id_param(params, "parent_id");
      size_t nl = 0;
      const char *tname = pl_str_param(params, "name", &nl);
      if (!ccord_contrib_ui_navigate(m->contrib, gid, cid, pid, tname ? tname : "", "channel") && m->session) {
        if (pid != CCORD_ID_NONE && gid != CCORD_ID_NONE) {
          (void)ccord_session_open_thread(m->session, gid, pid, cid, tname ? tname : "", &err);
        } else {
          (void)ccord_session_select_channel(m->session, cid, &err);
        }
      }
    }
    pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "presence.set") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_PRESENCE_WRITE)) { pl_fail_plugin(m, p, "undeclared restricted API called: presence.write"); return; }
    size_t sl = 0, tl = 0;
    const char *st_s = pl_str_param(params, "status", &sl);
    const char *txt = pl_str_param(params, "text", &tl);
    ccord_presence pr = CCORD_PRESENCE_ONLINE;
    if (!st_s || strcmp(st_s, "online") == 0) pr = CCORD_PRESENCE_ONLINE;
    else if (strcmp(st_s, "idle") == 0) pr = CCORD_PRESENCE_IDLE;
    else if (strcmp(st_s, "dnd") == 0) pr = CCORD_PRESENCE_DND;
    else if (strcmp(st_s, "invisible") == 0 || strcmp(st_s, "offline") == 0) pr = CCORD_PRESENCE_OFFLINE;
    else { pl_reply_err(m, p, id, "invalid_argument", "presence.set: status must be online|idle|dnd|invisible"); return; }
    ccord_error err = {0};
    if (!m->session || ccord_session_set_presence(m->session, pr, txt ? txt : "", &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "presence update failed");
    } else {
      pl_reply_ok(m, p, id, nullptr);
    }
    return;
  }
  if (strncmp(method, "fs.", 3) == 0) {
    if (strcmp(method, "fs.watch") == 0 || strcmp(method, "fs.unwatch") == 0 || strcmp(method, "fs.stat") == 0) {
      (void)plugins_ext_handle_request(m, p, msg);
      return;
    }
    req_fs(m, p, id, method, params);
    return;
  }
  if (strcmp(method, "voice.join") == 0 || strcmp(method, "voice.leave") == 0 ||
      strcmp(method, "voice.set_mute") == 0 || strcmp(method, "voice.set_deafen") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_VOICE_CONTROL)) { pl_fail_plugin(m, p, "undeclared restricted API called: voice.control"); return; }
    ccord_error err = {0};
    if (m->session) {
      if (strcmp(method, "voice.join") == 0) (void)ccord_session_join_voice(m->session, pl_parse_id_param(params, "guild_id"), pl_parse_id_param(params, "channel_id"), &err);
      else if (strcmp(method, "voice.leave") == 0) (void)ccord_session_leave_voice(m->session, pl_parse_id_param(params, "guild_id"), &err);
      else if (strcmp(method, "voice.set_mute") == 0) (void)ccord_session_set_voice_mute(m->session, ccord_json_bool(ccord_json_get(params, "muted"), false), &err);
      else (void)ccord_session_set_voice_deafen(m->session, ccord_json_bool(ccord_json_get(params, "deafened"), false), &err);
    }
    pl_reply_ok(m, p, id, nullptr);
    return;
  }

  // Class 1 (Safe) mobile UX APIs: gracefully report performed=false when unsupported on desktop.
  if (strcmp(method, "ui.haptics.impact") == 0) {
    size_t sl = 0;
    const char *style = pl_str_param(params, "style", &sl);
    int code = 0;
    if (style && strcmp(style, "medium") == 0) code = 1;
    else if (style && strcmp(style, "heavy") == 0) code = 2;
    else if (style && strcmp(style, "selection") == 0) code = 3;
    bool ok = m->plat_ops.haptic_impact && m->plat_ops.haptic_impact(m->plat_ops.ud, code);
    pl_reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }
  if (strcmp(method, "ui.haptics.notify") == 0) {
    size_t kl = 0;
    const char *kind = pl_str_param(params, "kind", &kl);
    int code = 0;
    if (kind && strcmp(kind, "warning") == 0) code = 1;
    else if (kind && strcmp(kind, "error") == 0) code = 2;
    bool ok = m->plat_ops.haptic_notify && m->plat_ops.haptic_notify(m->plat_ops.ud, code);
    pl_reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }
  if (strcmp(method, "ui.share.text") == 0) {
    size_t tl = 0, xl = 0;
    const char *title = pl_str_param(params, "title", &tl);
    const char *text = pl_str_param(params, "text", &xl);
    if (!text || xl == 0 || xl > MAX_CLIPBOARD_BYTES) {
      pl_reply_err(m, p, id, "invalid_argument", "share text invalid");
      return;
    }
    bool ok = m->plat_ops.share_text && m->plat_ops.share_text(m->plat_ops.ud, title ? title : "", text);
    pl_reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }
  if (strcmp(method, "ui.keyboard.dismiss") == 0) {
    if (m->plat_ops.keyboard_dismiss) m->plat_ops.keyboard_dismiss(m->plat_ops.ud);
    pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "ui.mobile.keep_screen_on") == 0) {
    bool enabled = ccord_json_bool(ccord_json_get(params, "enabled"), false);
    bool ok = m->plat_ops.keep_screen_on && m->plat_ops.keep_screen_on(m->plat_ops.ud, enabled);
    pl_reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }
  if (strcmp(method, "ui.mobile.set_orientation") == 0) {
    size_t ml = 0;
    const char *mode = pl_str_param(params, "mode", &ml);
    int code = 0;
    if (mode && strcmp(mode, "portrait") == 0) code = 1;
    else if (mode && strcmp(mode, "landscape") == 0) code = 2;
    bool ok = m->plat_ops.set_orientation && m->plat_ops.set_orientation(m->plat_ops.ud, code);
    pl_reply_ok(m, p, id, ok ? "{\"performed\":true}" : "{\"performed\":false}");
    return;
  }

  // Class 2 / Class 3 system & clipboard bridges.
  if (strcmp(method, "clipboard.read") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_CLIPBOARD_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: clipboard.read"); return; }
    char *txt = m->plat_ops.clipboard_read ? m->plat_ops.clipboard_read(m->plat_ops.ud) : nullptr;
    if (!txt) {
      pl_reply_ok(m, p, id, "{\"text\":null}");
      return;
    }
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "text"); ccord_jw_string(&w, txt, strlen(txt));
    ccord_jw_end_object(&w);
    ccord_free(txt);
    if (ccord_jw_ok(&w)) pl_reply_ok(m, p, id, ccord_buf_cstr(&b));
    else pl_reply_err(m, p, id, "internal", "clipboard text encode failed");
    ccord_buf_free(&b);
    return;
  }
  if (strcmp(method, "system.open_url") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_OPEN)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.open"); return; }
    size_t ul = 0;
    const char *url = pl_str_param(params, "url", &ul);
    if (!url || (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)) {
      pl_reply_err(m, p, id, "invalid_argument", "only http:// and https:// URLs allowed");
      return;
    }
    if (m->plat_ops.open_url) (void)m->plat_ops.open_url(m->plat_ops.ud, url);
    pl_reply_ok(m, p, id, nullptr);
    return;
  }
  if (strcmp(method, "system.notify") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SYSTEM_NOTIFY)) { pl_fail_plugin(m, p, "undeclared restricted API called: system.notify"); return; }
    size_t tl = 0, bl = 0, tagl = 0;
    const char *title = pl_str_param(params, "title", &tl);
    const char *body = pl_str_param(params, "body", &bl);
    const char *tag = pl_str_param(params, "tag", &tagl);
    bool silent = ccord_json_bool(ccord_json_get(params, "silent"), false);
    if (m->plat_ops.notify) (void)m->plat_ops.notify(m->plat_ops.ud, title ? title : "", body ? body : "", tag, silent);
    pl_reply_ok(m, p, id, nullptr);
    return;
  }

  if (plugins_ext_handle_request(m, p, msg)) return;
  pl_reply_err(m, p, id, "unsupported", "unknown method");
}

// ---------------------------------------------------------------- host -> main notifications and responses

static void handle_hello(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  const ccord_json *proto = msg->params ? ccord_json_get(msg->params, "proto") : nullptr;
  size_t ll = 0;
  const char *lua = pl_str_param(msg->params, "lua", &ll);
  int64_t pv = 0;
  if (!proto || !ccord_json_int64(proto, &pv) || pv != CCORD_IPC_PROTO_VERSION) {
    { pl_fail_plugin(m, p, "hello: protocol version mismatch"); return; }
  }
  if (!lua || strcmp(lua, CCORD_LUA_EXPECTED_RELEASE) != 0) {
    { pl_fail_plugin(m, p, "hello: host runs an unexpected Lua release"); return; }
  }
  if (p->hello_seen) { pl_fail_plugin(m, p, "hello sent twice"); return; }
  p->hello_seen = true;

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "plugin");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, p->m.id);
  ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, p->m.name);
  ccord_jw_key(&w, "version"); ccord_jw_cstr(&w, p->m.version);
  ccord_jw_key(&w, "dir"); ccord_jw_cstr(&w, p->dir ? p->dir : "");
  ccord_jw_end_object(&w);
  ccord_jw_key(&w, "permissions");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < ccord_perm_count(); i++) {
    ccord_perm pm = ccord_perm_at(i);
    if (pl_has_perm(p, pm)) ccord_jw_cstr(&w, ccord_perm_name(pm));
  }
  ccord_jw_end_array(&w);
  ccord_jw_key(&w, "source"); ccord_jw_string(&w, p->source ? p->source : "", p->source ? p->source_len : 0);
  ccord_jw_key(&w, "api"); ccord_jw_int(&w, CCORD_PLUGIN_API_VERSION);
  if (p->m.memory_bytes > 0 || p->m.instructions > 0) {
    ccord_jw_key(&w, "limits");
    ccord_jw_begin_object(&w);
    if (p->m.memory_bytes > 0) { ccord_jw_key(&w, "memory_bytes"); ccord_jw_int(&w, (int64_t)p->m.memory_bytes); }
    if (p->m.instructions > 0) { ccord_jw_key(&w, "instructions"); ccord_jw_int(&w, (int64_t)p->m.instructions); }
    ccord_jw_end_object(&w);
  }
  ccord_jw_key(&w, "platform");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "os"); ccord_jw_cstr(&w, m->plat_os);
  ccord_jw_key(&w, "arch"); ccord_jw_cstr(&w, m->plat_arch);
  ccord_jw_key(&w, "form_factor"); ccord_jw_cstr(&w, m->plat_form_factor);
  ccord_jw_key(&w, "is_mobile"); ccord_jw_bool(&w, m->plat_is_mobile);
  ccord_jw_key(&w, "touch"); ccord_jw_bool(&w, m->plat_touch);
  ccord_jw_key(&w, "android_api_level"); ccord_jw_int(&w, m->plat_android_api_level);
  ccord_jw_key(&w, "capabilities_mask"); ccord_jw_int(&w, (int64_t)m->plat_capabilities);
  ccord_jw_key(&w, "state");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "safe_top"); ccord_jw_int(&w, (int64_t)m->plat_state.safe_top);
  ccord_jw_key(&w, "safe_right"); ccord_jw_int(&w, (int64_t)m->plat_state.safe_right);
  ccord_jw_key(&w, "safe_bottom"); ccord_jw_int(&w, (int64_t)m->plat_state.safe_bottom);
  ccord_jw_key(&w, "safe_left"); ccord_jw_int(&w, (int64_t)m->plat_state.safe_left);
  ccord_jw_key(&w, "viewport_w"); ccord_jw_int(&w, (int64_t)m->plat_state.viewport_w);
  ccord_jw_key(&w, "viewport_h"); ccord_jw_int(&w, (int64_t)m->plat_state.viewport_h);
  ccord_jw_key(&w, "viewport_scale"); ccord_jw_double(&w, (double)(m->plat_state.viewport_scale > 0.0f ? m->plat_state.viewport_scale : 1.0f));
  ccord_jw_key(&w, "keyboard_height"); ccord_jw_int(&w, (int64_t)m->plat_state.keyboard_height);
  ccord_jw_key(&w, "keyboard_visible"); ccord_jw_bool(&w, m->plat_state.keyboard_visible);
  ccord_jw_key(&w, "landscape"); ccord_jw_bool(&w, m->plat_state.landscape);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (p->nitems > 0) {
    ccord_arena ar;
    ccord_arena_init(&ar, 0);
    ccord_jw_key(&w, "storage");
    ccord_jw_begin_object(&w);
    for (size_t i = 0; i < p->nitems; i++) {
      ccord_arena_reset(&ar);
      ccord_error pe = {0};
      const ccord_json *v = ccord_json_parse(&ar, p->items[i].json, p->items[i].json_len, &pl_json_limits, &pe);
      ccord_jw_key(&w, p->items[i].key);
      pl_jw_copy_json(&w, v);
    }
    ccord_jw_end_object(&w);
    ccord_arena_free(&ar);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_free(&b);
    { pl_fail_plugin(m, p, "main.lua is not valid UTF-8 text"); return; }
  }
  pending_req *slot = pl_pend_free(p);
  if (!slot) {
    ccord_buf_free(&b);
    { pl_fail_plugin(m, p, "internal: no free request slot"); return; }
  }
  uint32_t id = pl_next_request_id(p);
  ccord_error err = {0};
  if (ccord_ipc_send_request(p->ipc, id, "init", ccord_buf_cstr(&b), &err) != CCORD_OK) {
    ccord_buf_free(&b);
    char why[256];
    snprintf(why, sizeof why, "could not send init: %s", err.message);
    { pl_fail_plugin(m, p, why); return; }
  }
  ccord_buf_free(&b);
  // Source has been queued for the plugin host; drop our copy to avoid retaining it in RAM.
  ccord_free(p->source);
  p->source = nullptr;
  p->source_len = 0;
  slot->kind = PEND_INIT;
  slot->id = id;
  slot->deadline_ms = m->now_ms + INIT_DEADLINE_MS;
  (void)ccord_ipc_flush(p->ipc, 0, &err);
}

static void handle_log(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg, uint64_t now)
{
  size_t ml = 0, ll = 0;
  const char *level = pl_str_param(msg->params, "level", &ll);
  const char *text = pl_str_param(msg->params, "msg", &ml);
  ccord_log_level lvl = CCORD_LOG_INFO;
  if (level && strcmp(level, "debug") == 0) lvl = CCORD_LOG_DEBUG;
  else if (level && strcmp(level, "warn") == 0) lvl = CCORD_LOG_WARN;
  else if (level && strcmp(level, "error") == 0) lvl = CCORD_LOG_ERROR;
  char clipped[1024];
  snprintf(clipped, sizeof clipped, "%.*s", (int)(ml < 1000 ? ml : 1000), text ? text : "");
  ccord_logf(lvl, __FILE__, __LINE__, "[plugin %s] %s", p->m.id, clipped);
  ccord_contrib_add_plugin_log(m->contrib, p->m.id, now, (uint8_t)lvl, clipped);
}

static void handle_response(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  pending_req *slot = nullptr;
  for (size_t i = 0; i < MAX_PENDING; i++) {
    if (p->pend[i].kind != PEND_NONE && p->pend[i].id == msg->id) slot = &p->pend[i];
  }
  if (!slot) { pl_fail_plugin(m, p, "protocol violation: response for unknown request"); return; }
  pend_kind kind = slot->kind;
  ccord_id channel = slot->channel_id;
  slot->kind = PEND_NONE;
  if (kind == PEND_INIT) {
    if (!msg->ok) {
      char why[256];
      snprintf(why, sizeof why, "init failed: %s", msg->err_message ? msg->err_message : "unknown error");
      { pl_fail_plugin(m, p, why); return; }
    }
    p->state = CCORD_PLUGIN_READY;
    p->last_ping_sent_ms = m->now_ms;
    CCORD_LOG_I("plugin %s ready", p->m.id);
    pl_publish_state(m, p);
    if (m->in_background && p->ipc) {
      pl_send_ntf(m, p, "host.background", "{\"background\":true}");
    }
  } else if (kind == PEND_CMD) {
    if (!msg->ok) {
      CCORD_LOG_W("plugin %s: command failed: %s", p->m.id, msg->err_message ? msg->err_message : "");
      return;
    }
    plugins_ext_handle_command_result(m, p, channel, msg->result);
  }
  // PEND_PING: the answer itself is the liveness signal.
}

void pl_handle_frame(ccord_plugins *m, plugin *p, const ccord_ipc_msg *msg)
{
  if (msg->kind == CCORD_IPC_NTF) {
    if (strcmp(msg->method, "hello") == 0) { handle_hello(m, p, msg); return; }
    if (strcmp(msg->method, "log") == 0) { handle_log(m, p, msg, m->now_ms); return; }
    CCORD_LOG_D("plugin %s: ignored notification %s", p->m.id, msg->method);
    return;
  }
  if (msg->kind == CCORD_IPC_REQ) { handle_request(m, p, msg); return; }
  { handle_response(m, p, msg); return; }
}

