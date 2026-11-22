// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "api_v3_internal.h"

// ---------------------------------------------------------------- Public helper utilities

void plugins_ext_reset_plugin(plugin *p)
{
  if (!p) return;
  p->hook_mask = 0;
  p->gw_raw_any = false;
  for (size_t i = 0; i < p->ngw_raw_events; i++) ccord_free(p->gw_raw_events[i]);
  ccord_free(p->gw_raw_events);
  p->gw_raw_events = nullptr;
  p->ngw_raw_events = 0;
  p->voice_pcm_sub = false;
  ccord_free(p->exposed);
  p->exposed = nullptr;
  p->nexposed = 0;
}

void plugins_ext_format_allowed_hosts(const ccord_manifest *man, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!man || man->allowed_host_count == 0) return;
  size_t pos = 0;
  for (size_t i = 0; i < man->allowed_host_count; i++) {
    int n = snprintf(out + pos, cap - pos, "%s%s", i ? ", " : "", man->allowed_hosts[i]);
    if (n < 0 || (size_t)n >= cap - pos) break;
    pos += (size_t)n;
  }
}

bool plugins_ext_event_bit_for(const char *name, uint64_t *bit, ccord_perm *perm)
{
  if (strcmp(name, "theme.change") == 0) { *bit = EVB_THEME_CHANGE; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "window.focus") == 0) { *bit = EVB_WINDOW_FOCUS; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "window.resize") == 0) { *bit = EVB_WINDOW_RESIZE; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "streamer_mode.change") == 0) { *bit = EVB_STREAMER_MODE; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "settings.change") == 0) { *bit = EVB_SETTINGS_CHANGE; *perm = CCORD_PERM_EVENTS_NAVIGATION; return true; }
  if (strcmp(name, "composer.change") == 0) { *bit = EVB_COMPOSER_CHANGE; *perm = CCORD_PERM_UI_COMPOSER; return true; }
  if (strcmp(name, "voice.levels") == 0) { *bit = EVB_VOICE_LEVELS; *perm = CCORD_PERM_VOICE_LEVELS; return true; }
  return false;
}

void plugins_ext_write_message_obj(ccord_jw *w, const ccord_store *st, const ccord_message *msg)
{
  const ccord_user *u = st ? ccord_store_user(st, msg->author_id) : nullptr;
  ccord_jw_begin_object(w);
  ccord_jw_key(w, "id"); ccord_jw_id(w, msg->id);
  ccord_jw_key(w, "channel_id"); ccord_jw_id(w, msg->channel_id);
  ccord_jw_key(w, "author_id"); ccord_jw_id(w, msg->author_id);
  ccord_jw_key(w, "author_name"); ccord_jw_cstr(w, u && u->name ? u->name : "");
  ccord_jw_key(w, "content"); ccord_jw_cstr(w, msg->content ? msg->content : "");
  ccord_jw_key(w, "timestamp_ms"); ccord_jw_int(w, msg->timestamp_ms);
  ccord_jw_key(w, "edited"); ccord_jw_bool(w, msg->edited);
  ccord_jw_key(w, "pinned"); ccord_jw_bool(w, msg->pinned);
  if (msg->reply_to_id) { ccord_jw_key(w, "reply_to_id"); ccord_jw_id(w, msg->reply_to_id); }
  if (msg->reaction_kind_count > 0 && msg->reactions) {
    ccord_jw_key(w, "reactions");
    ccord_jw_begin_array(w);
    for (size_t i = 0; i < msg->reaction_kind_count; i++) {
      ccord_jw_begin_object(w);
      ccord_jw_key(w, "emoji"); ccord_jw_cstr(w, msg->reactions[i].emoji ? msg->reactions[i].emoji : "");
      ccord_jw_key(w, "count"); ccord_jw_int(w, (int64_t)msg->reactions[i].count);
      ccord_jw_key(w, "me"); ccord_jw_bool(w, msg->reactions[i].self_reacted);
      ccord_jw_end_object(w);
    }
    ccord_jw_end_array(w);
  }
  if (msg->rich) {
    const ccord_message_rich *r = msg->rich;
    ccord_jw_key(w, "flags"); ccord_jw_int(w, (int64_t)r->flags);
    ccord_jw_key(w, "type"); ccord_jw_int(w, (int64_t)r->type);
    if (r->attachment_count > 0 && r->attachments) {
      ccord_jw_key(w, "attachments");
      ccord_jw_begin_array(w);
      for (size_t i = 0; i < r->attachment_count; i++) {
        ccord_jw_begin_object(w);
        ccord_jw_key(w, "id"); ccord_jw_id(w, r->attachments[i].id);
        ccord_jw_key(w, "filename"); ccord_jw_cstr(w, r->attachments[i].filename ? r->attachments[i].filename : "");
        ccord_jw_key(w, "content_type"); ccord_jw_cstr(w, r->attachments[i].content_type ? r->attachments[i].content_type : "");
        ccord_jw_key(w, "url"); ccord_jw_cstr(w, r->attachments[i].url ? r->attachments[i].url : "");
        ccord_jw_key(w, "size"); ccord_jw_int(w, (int64_t)r->attachments[i].size);
        ccord_jw_key(w, "width"); ccord_jw_int(w, (int64_t)r->attachments[i].width);
        ccord_jw_key(w, "height"); ccord_jw_int(w, (int64_t)r->attachments[i].height);
        ccord_jw_end_object(w);
      }
      ccord_jw_end_array(w);
    }
    if (r->embed_count > 0 && r->embeds) {
      ccord_jw_key(w, "embeds");
      ccord_jw_begin_array(w);
      for (size_t i = 0; i < r->embed_count; i++) {
        ccord_jw_begin_object(w);
        ccord_jw_key(w, "title"); ccord_jw_cstr(w, r->embeds[i].title ? r->embeds[i].title : "");
        ccord_jw_key(w, "description"); ccord_jw_cstr(w, r->embeds[i].description ? r->embeds[i].description : "");
        ccord_jw_key(w, "url"); ccord_jw_cstr(w, r->embeds[i].url ? r->embeds[i].url : "");
        if (r->embeds[i].has_color) { ccord_jw_key(w, "color"); ccord_jw_int(w, (int64_t)r->embeds[i].color); }
        ccord_jw_end_object(w);
      }
      ccord_jw_end_array(w);
    }
  }
  ccord_jw_end_object(w);
}

void plugins_ext_handle_command_result(ccord_plugins *m, plugin *p, ccord_id channel, const ccord_json *result)
{
  if (!m || !p || !result) return;
  bool local_notice = ccord_json_bool(ccord_json_get(result, "local_notice"), false);
  size_t tl = 0;
  const char *text = pl_str_param(result, "text", &tl);
  if (local_notice) {
    ccord_buf emb_buf; ccord_buf_init(&emb_buf, 0);
    const ccord_json *ej = ccord_json_get(result, "embeds");
    if (ej) ext_encode_json_str(ej, &emb_buf);
    ccord_local_message lm = {
      .owner = p->m.id,
      .channel_id = channel,
      .author_name = p->m.name,
      .author_badge = "PLUGIN",
      .content = text ? text : "",
      .embed_json = emb_buf.len ? ccord_buf_cstr(&emb_buf) : "",
      .timestamp_ms = (uint64_t)ccord_clock_unix_ms(),
      .dismissible = true,
    };
    ccord_error err = {0};
    (void)ccord_contrib_add_local_message(m->contrib, &lm, nullptr, &err);
    ccord_buf_free(&emb_buf);
    return;
  }
  if (!text) return;
  if (!pl_has_perm(p, CCORD_PERM_MESSAGES_SEND)) {
    if (!ccord_contrib_ui_insert_draft(m->contrib, channel, text)) {
      CCORD_LOG_W("plugin %s: command result requires messages.send permission or an attached composer", p->m.id);
    }
    return;
  }
  if (!m->session) return;
  ccord_error err = {0};
  if (ccord_session_send_message(m->session, channel, text, tl, &err) != CCORD_OK) {
    CCORD_LOG_W("plugin %s: command result not sent: %s", p->m.id, err.message);
  }
}

bool ext_handle_audio_hooks_gw_rpc(ccord_plugins *m, plugin *p, uint32_t id, const char *method,
                                      const ccord_json *params)
{
  if (strcmp(method, "audio.play_earcon") == 0) {
    size_t kl = 0; const char *kind = pl_str_param(params, "kind", &kl);
    if (kind) (void)ccord_contrib_ui_play_earcon(m->contrib, kind);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "audio.play_bytes") == 0) {
    size_t bl = 0; const char *b64 = pl_str_param(params, "b64", &bl);
    size_t cap = (bl / 4u) * 3u + 4u;
    uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
    size_t rlen = 0;
    if (raw && pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen)) {
      (void)ccord_contrib_ui_play_audio_bytes(m->contrib, raw, rlen);
    }
    ccord_free(raw);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "audio.override_earcon") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_AUDIO_EARCONS)) { pl_fail_plugin(m, p, "undeclared restricted API called: audio.earcons"); return true; }
    size_t kl = 0, bl = 0;
    const char *kind = pl_str_param(params, "kind", &kl);
    const char *b64 = pl_str_param(params, "b64", &bl);
    size_t cap = (bl / 4u) * 3u + 4u;
    uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
    size_t rlen = 0;
    ccord_error err = {0};
    if (!kind || !raw || !pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen) ||
        ccord_contrib_set_earcon_override(m->contrib, p->m.id, kind, raw, rlen, &err) != CCORD_OK) {
      ccord_free(raw);
      pl_reply_err(m, p, id, "invalid_argument", err.message[0] ? err.message : "earcon override failed");
      return true;
    }
    ccord_free(raw);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "audio.reset_earcon") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_AUDIO_EARCONS)) { pl_fail_plugin(m, p, "undeclared restricted API called: audio.earcons"); return true; }
    size_t kl = 0; const char *kind = pl_str_param(params, "kind", &kl);
    if (kind) (void)ccord_contrib_remove_earcon_override(m->contrib, p->m.id, kind);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "hooks.register") == 0 || strcmp(method, "hooks.unregister") == 0) {
    bool reg = (strcmp(method, "hooks.register") == 0);
    size_t hl = 0; const char *hk = pl_str_param(params, "hook", &hl);
    uint8_t bit = 0; ccord_perm need = CCORD_PERM_MESSAGES_HOOK;
    if (hk && strcmp(hk, "before_send") == 0) { bit = HOOK_BEFORE_SEND; need = CCORD_PERM_MESSAGES_HOOK; }
    else if (hk && strcmp(hk, "before_edit") == 0) { bit = HOOK_BEFORE_EDIT; need = CCORD_PERM_MESSAGES_HOOK; }
    else if (hk && strcmp(hk, "notification") == 0) { bit = HOOK_NOTIFICATION; need = CCORD_PERM_NOTIFICATIONS_HOOK; }
    else if (hk && strcmp(hk, "link_open") == 0) { bit = HOOK_LINK_OPEN; need = CCORD_PERM_NOTIFICATIONS_HOOK; }
    else { pl_reply_err(m, p, id, "invalid_argument", "unknown hook"); return true; }
    if (!pl_has_perm(p, need)) { pl_fail_plugin(m, p, "undeclared restricted API called: hooks"); return true; }
    if (reg) p->hook_mask |= bit; else p->hook_mask &= (uint8_t)~bit;
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "gateway.subscribe") == 0 || strcmp(method, "gateway.unsubscribe") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_GATEWAY_RAW)) { pl_fail_plugin(m, p, "undeclared restricted API called: gateway.raw"); return true; }
    bool sub = (strcmp(method, "gateway.subscribe") == 0);
    size_t el = 0; const char *ev = pl_str_param(params, "event", &el);
    if (!ev || el == 0) { pl_reply_err(m, p, id, "invalid_argument", "missing event"); return true; }
    if (strcmp(ev, "*") == 0) {
      p->gw_raw_any = sub;
    } else if (sub) {
      bool exists = false;
      for (size_t i = 0; i < p->ngw_raw_events; i++) {
        if (strcmp(p->gw_raw_events[i], ev) == 0) { exists = true; break; }
      }
      if (!exists && p->ngw_raw_events < 64u) {
        char **na = ccord_realloc_array(p->gw_raw_events, p->ngw_raw_events + 1u, sizeof *na);
        char *cp = ccord_strdup(ev);
        if (na && cp) {
          p->gw_raw_events = na;
          p->gw_raw_events[p->ngw_raw_events++] = cp;
        } else {
          ccord_free(cp);
        }
      }
    } else {
      for (size_t i = 0; i < p->ngw_raw_events; i++) {
        if (strcmp(p->gw_raw_events[i], ev) == 0) {
          ccord_free(p->gw_raw_events[i]);
          memmove(&p->gw_raw_events[i], &p->gw_raw_events[i + 1], (p->ngw_raw_events - i - 1u) * sizeof p->gw_raw_events[0]);
          p->ngw_raw_events--;
          break;
        }
      }
    }
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "plugin.expose") == 0) {
    size_t nl = 0; const char *name = pl_str_param(params, "name", &nl);
    bool allow_low = ccord_json_bool(ccord_json_get(params, "allow_lower_class"), false);
    if (!name || nl == 0 || nl >= 64u) { pl_reply_err(m, p, id, "invalid_argument", "invalid function name"); return true; }
    for (size_t i = 0; i < p->nexposed; i++) {
      if (strcmp(p->exposed[i].name, name) == 0) {
        p->exposed[i].allow_lower_class = allow_low;
        pl_reply_ok(m, p, id, nullptr);
        return true;
      }
    }
    if (p->nexposed < 64u) {
      plugin_exposed_fn *na = ccord_realloc_array(p->exposed, p->nexposed + 1u, sizeof *na);
      if (na) {
        p->exposed = na;
        pl_set_text(p->exposed[p->nexposed].name, sizeof p->exposed[0].name, name);
        p->exposed[p->nexposed].allow_lower_class = allow_low;
        p->nexposed++;
      }
    }
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "plugin.list_exposed") == 0) {
    size_t tl = 0; const char *target = pl_str_param(params, "target", &tl);
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    for (size_t i = 0; i < m->nplugins; i++) {
      const plugin *tp = m->plugins[i];
      if (!tp->used || tp->state != CCORD_PLUGIN_READY || tp->nexposed == 0) continue;
      if (target && target[0] && strcmp(tp->m.id, target) != 0) continue;
      ccord_jw_key(&w, tp->m.id);
      ccord_jw_begin_array(&w);
      for (size_t k = 0; k < tp->nexposed; k++) ccord_jw_cstr(&w, tp->exposed[k].name);
      ccord_jw_end_array(&w);
    }
    ccord_jw_end_object(&w);
    pl_reply_ok(m, p, id, ccord_jw_ok(&w) ? ccord_buf_cstr(&b) : "{}");
    ccord_buf_free(&b);
    return true;
  }
  if (strcmp(method, "plugin.call") == 0) {
    int64_t req_id = 0; (void)ccord_json_int64(ccord_json_get(params, "req_id"), &req_id);
    size_t tl = 0, nl = 0;
    const char *target_id = pl_str_param(params, "target", &tl);
    const char *fn_name = pl_str_param(params, "name", &nl);
    const ccord_json *args = ccord_json_get(params, "args");
    pl_reply_ok(m, p, id, nullptr);
    plugin *tp = target_id ? pl_find_owner(m, target_id) : nullptr;
    if (!tp || tp->state != CCORD_PLUGIN_READY || !fn_name) {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, "target plugin not ready");
      return true;
    }
    // Anti-escalation check enforced on main before forwarding!
    int caller_cls = (int)ccord_perm_max_class(p->m.perms);
    int target_cls = (int)ccord_perm_max_class(tp->m.perms);
    bool found = false, allow_low = false;
    for (size_t i = 0; i < tp->nexposed; i++) {
      if (strcmp(tp->exposed[i].name, fn_name) == 0) {
        found = true;
        allow_low = tp->exposed[i].allow_lower_class;
        break;
      }
    }
    if (!found) {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, "exposed function not found");
      return true;
    }
    if (caller_cls < target_cls && !allow_low) {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr,
                       "permission denied: lower-class plugin cannot call higher-class exposed function");
      return true;
    }
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, fn_name);
    ccord_jw_key(&w, "caller_id"); ccord_jw_cstr(&w, p->m.id);
    ccord_jw_key(&w, "caller_class"); ccord_jw_int(&w, caller_cls);
    ccord_jw_key(&w, "args");
    if (args) pl_jw_copy_json(&w, args); else { ccord_jw_begin_array(&w); ccord_jw_end_array(&w); }
    ccord_jw_end_object(&w);
    char *out_json = nullptr; char ebuf[256] = "";
    bool ok = ccord_jw_ok(&w) && ext_sync_host_call(m, tp, "plugin.rpc.call", ccord_buf_cstr(&b), 500u, &out_json, ebuf, sizeof ebuf);
    ccord_buf_free(&b);
    ext_send_async_reply(m, p, (uint64_t)req_id, ok, out_json, ebuf);
    ccord_free(out_json);
    return true;
  }
  return false;
}

bool ext_handle_messages_store_v3(ccord_plugins *m, plugin *p, uint32_t id, const char *method,
                                     const ccord_json *params)
{
  if (strncmp(method, "messages.", 9) == 0) {
    ccord_error err = {0};
    if (strcmp(method, "messages.reply") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_SEND)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.send"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id"), rid = pl_parse_id_param(params, "reply_to_id");
      size_t cl = 0; const char *cnt = pl_str_param(params, "content", &cl);
      bool ping = ccord_json_bool(ccord_json_get(params, "mention_author"), true);
      if (!m->session || !cid || !rid || !cnt ||
          ccord_session_send_reply_ex(m->session, cid, rid, ping, cnt, cl, &err) != CCORD_OK) {
        pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "reply failed");
      } else pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.react") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id"), mid = pl_parse_id_param(params, "message_id");
      size_t el = 0; const char *em = pl_str_param(params, "emoji", &el);
      if (!m->session || !cid || !mid || !em ||
          ccord_session_toggle_reaction(m->session, cid, mid, em, el, &err) != CCORD_OK) {
        pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "react failed");
      } else pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.pin") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id"), mid = pl_parse_id_param(params, "message_id");
      if (!m->session || !cid || !mid || ccord_session_toggle_pin(m->session, cid, mid, &err) != CCORD_OK) {
        pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "pin failed");
      } else pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.fetch_pins") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_STORE_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: store.read"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      if (m->session && cid) (void)ccord_session_request_pins(m->session, cid, &err);
      pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.fetch_history") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_STORE_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: store.read"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id"), bid = pl_parse_id_param(params, "before_id");
      if (m->session && cid) (void)ccord_session_request_history_before(m->session, cid, bid, &err);
      pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.search") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_STORE_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: store.read"); return true; }
      ccord_id gid = pl_parse_id_param(params, "guild_id"), cid = pl_parse_id_param(params, "channel_id");
      size_t ql = 0; const char *q = pl_str_param(params, "query", &ql);
      if (m->session && gid && q) (void)ccord_session_search_messages(m->session, gid, cid, q, ql, &err);
      ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
      ccord_jw_begin_array(&w);
      size_t n = m->session ? ccord_session_search_result_count(m->session) : 0;
      for (size_t i = 0; i < n; i++) {
        const ccord_message *msg = ccord_session_search_result_at(m->session, i);
        if (msg) plugins_ext_write_message_obj(&w, m->store, msg);
      }
      ccord_jw_end_array(&w);
      pl_reply_ok(m, p, id, ccord_jw_ok(&w) ? ccord_buf_cstr(&b) : "[]");
      ccord_buf_free(&b);
      return true;
    }
    if (strcmp(method, "messages.forward") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_SEND)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.send"); return true; }
      ccord_id tcid = pl_parse_id_param(params, "target_channel_id");
      ccord_id scid = pl_parse_id_param(params, "source_channel_id");
      ccord_id mid = pl_parse_id_param(params, "message_id");
      if (!m->session || ccord_session_forward_message(m->session, tcid, scid, mid, &err) != CCORD_OK) {
        pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "forward failed");
      } else pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.send_file") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_SEND)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.send"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      size_t fnl = 0, bl = 0, cl = 0;
      const char *fname = pl_str_param(params, "filename", &fnl);
      const char *b64 = pl_str_param(params, "b64", &bl);
      const char *cnt = pl_str_param(params, "content", &cl);
      size_t cap = (bl / 4u) * 3u + 4u;
      uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
      size_t rlen = 0;
      if (!m->session || !cid || !fname || !raw || !pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen) ||
          ccord_session_send_message_with_file(m->session, cid, cnt ? cnt : "", cl, fname, raw, rlen, &err) != CCORD_OK) {
        ccord_free(raw);
        pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "send_file failed");
        return true;
      }
      ccord_free(raw);
      pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.send_poll") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_SEND)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.send"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      const ccord_json *pj = ccord_json_get(params, "poll");
      size_t ql = 0; const char *q = pj ? pl_str_param(pj, "question", &ql) : nullptr;
      const ccord_json *aj = pj ? ccord_json_get(pj, "answers") : nullptr;
      const char *ans[CCORD_RICH_MAX_POLL_ANSWERS];
      size_t na = 0;
      if (aj && ccord_json_type_of(aj) == CCORD_JSON_ARRAY) {
        size_t cnt = ccord_json_array_len(aj);
        for (size_t i = 0; i < cnt && na < CCORD_RICH_MAX_POLL_ANSWERS; i++) {
          size_t al = 0;
          const char *s = ccord_json_string(ccord_json_array_at(aj, i), &al);
          if (s && al > 0) ans[na++] = s;
        }
      }
      int64_t dur = 24;
      if (pj && ccord_json_get(pj, "duration_hours")) (void)ccord_json_int64(ccord_json_get(pj, "duration_hours"), &dur);
      ccord_poll_create pc = {
        .question = q ? q : "", .answers = ans, .answer_count = na,
        .duration_hours = (uint32_t)(dur > 0 ? dur : 24),
        .allow_multiselect = pj ? ccord_json_bool(ccord_json_get(pj, "allow_multiselect"), false) : false,
      };
      if (!m->session || !cid || ccord_session_send_poll(m->session, cid, &pc, &err) != CCORD_OK) {
        pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "send_poll failed");
      } else pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.ack") == 0 || strcmp(method, "messages.mark_unread") == 0 ||
        strcmp(method, "messages.save") == 0 || strcmp(method, "messages.unsave") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id"), mid = pl_parse_id_param(params, "message_id");
      if (m->session && cid && mid) {
        if (strcmp(method, "messages.ack") == 0) (void)ccord_session_ack_message(m->session, cid, mid, &err);
        else if (strcmp(method, "messages.mark_unread") == 0) (void)ccord_session_mark_unread(m->session, cid, mid, &err);
        else if (strcmp(method, "messages.save") == 0) (void)ccord_session_save_message(m->session, cid, mid, &err);
        else (void)ccord_session_unsave_message(m->session, cid, mid, &err);
      }
      pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "messages.saved") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_STORE_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: store.read"); return true; }
      ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
      ccord_jw_begin_array(&w);
      size_t n = m->session ? ccord_session_saved_message_count(m->session) : 0;
      for (size_t i = 0; i < n; i++) {
        const ccord_message *msg = ccord_session_saved_message_at(m->session, i);
        if (msg) plugins_ext_write_message_obj(&w, m->store, msg);
      }
      ccord_jw_end_array(&w);
      pl_reply_ok(m, p, id, ccord_jw_ok(&w) ? ccord_buf_cstr(&b) : "[]");
      ccord_buf_free(&b);
      return true;
    }
    if (strcmp(method, "messages.schedule") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_SEND)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.send"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      size_t cl = 0; const char *cnt = pl_str_param(params, "content", &cl);
      int64_t due = 0; (void)ccord_json_int64(ccord_json_get(params, "due_unix_ms"), &due);
      char eb[128] = "";
      if (!m->plat_ops.scheduled_add || !m->plat_ops.scheduled_add(m->plat_ops.ud, cid, cnt ? cnt : "", cl, due, eb, sizeof eb)) {
        pl_reply_err(m, p, id, "unsupported", eb[0] ? eb : "scheduled messages unavailable");
      } else pl_reply_ok(m, p, id, "true");
      return true;
    }
    if (strcmp(method, "messages.scheduled_list") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_STORE_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: store.read"); return true; }
      char *j = m->plat_ops.scheduled_list_json ? m->plat_ops.scheduled_list_json(m->plat_ops.ud) : nullptr;
      pl_reply_ok(m, p, id, j ? j : "[]");
      ccord_free(j);
      return true;
    }
    if (strcmp(method, "messages.scheduled_cancel") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_MESSAGES_MANAGE)) { pl_fail_plugin(m, p, "undeclared restricted API called: messages.manage"); return true; }
      int64_t sid = 0; (void)ccord_json_int64(ccord_json_get(params, "id"), &sid);
      bool ok = m->plat_ops.scheduled_cancel && m->plat_ops.scheduled_cancel(m->plat_ops.ud, (uint64_t)sid);
      pl_reply_ok(m, p, id, ok ? "true" : "false");
      return true;
    }
    if (strcmp(method, "messages.inject_local") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_UI_PANEL)) { pl_fail_plugin(m, p, "undeclared restricted API called: ui.panel"); return true; }
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      const ccord_json *mj = ccord_json_get(params, "msg");
      if (!cid || !mj || ccord_json_type_of(mj) != CCORD_JSON_OBJECT) {
        pl_reply_err(m, p, id, "invalid_argument", "channel_id and msg required");
        return true;
      }
      size_t al = 0, bl = 0, cl = 0;
      ccord_buf emb_b; ccord_buf_init(&emb_b, 0);
      if (ccord_json_get(mj, "embeds")) ext_encode_json_str(ccord_json_get(mj, "embeds"), &emb_b);
      else if (ccord_json_get(mj, "embed")) ext_encode_json_str(ccord_json_get(mj, "embed"), &emb_b);
      ccord_local_message lm = {
        .owner = p->m.id, .channel_id = cid,
        .author_name = pl_str_param(mj, "author", &al) ? pl_str_param(mj, "author", &al) : p->m.name,
        .author_badge = pl_str_param(mj, "badge", &bl) ? pl_str_param(mj, "badge", &bl) : "PLUGIN",
        .content = pl_str_param(mj, "content", &cl) ? pl_str_param(mj, "content", &cl) : "",
        .embed_json = emb_b.len ? ccord_buf_cstr(&emb_b) : "",
        .timestamp_ms = (uint64_t)ccord_clock_unix_ms(),
        .dismissible = ccord_json_bool(ccord_json_get(mj, "dismissible"), true),
      };
      uint64_t lid = 0;
      ccord_status st = ccord_contrib_add_local_message(m->contrib, &lm, &lid, &err);
      ccord_buf_free(&emb_b);
      if (st != CCORD_OK) { pl_reply_err(m, p, id, "invalid_argument", err.message); return true; }
      char buf[32];
      snprintf(buf, sizeof buf, "%llu", (unsigned long long)lid);
      pl_reply_ok(m, p, id, buf);
      return true;
    }
    if (strcmp(method, "messages.dismiss_local") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_UI_PANEL)) { pl_fail_plugin(m, p, "undeclared restricted API called: ui.panel"); return true; }
      int64_t lid = 0; (void)ccord_json_int64(ccord_json_get(params, "local_id"), &lid);
      (void)ccord_contrib_dismiss_local_message(m->contrib, (uint64_t)lid);
      pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    return false;
  }

  if (strncmp(method, "store.", 6) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_STORE_READ)) { pl_fail_plugin(m, p, "undeclared restricted API called: store.read"); return true; }
    if (!m->store) { pl_reply_err(m, p, id, "unsupported", "store unavailable"); return true; }
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    if (strcmp(method, "store.roles") == 0) {
      ccord_id gid = pl_parse_id_param(params, "guild_id");
      size_t n = 0;
      const ccord_role *rs = ccord_store_roles(m->store, gid, &n);
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < n; i++) {
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "id"); ccord_jw_id(&w, rs[i].id);
        ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, rs[i].name ? rs[i].name : "");
        ccord_jw_key(&w, "color"); ccord_jw_int(&w, (int64_t)rs[i].color);
        ccord_jw_key(&w, "position"); ccord_jw_int(&w, (int64_t)rs[i].position);
        ccord_jw_key(&w, "permissions"); ccord_jw_int(&w, (int64_t)rs[i].permissions);
        ccord_jw_key(&w, "hoist"); ccord_jw_bool(&w, rs[i].hoist);
        ccord_jw_key(&w, "mentionable"); ccord_jw_bool(&w, rs[i].mentionable);
        ccord_jw_end_object(&w);
      }
      ccord_jw_end_array(&w);
    } else if (strcmp(method, "store.has_permission") == 0) {
      ccord_id gid = pl_parse_id_param(params, "guild_id");
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      ccord_id uid = pl_parse_id_param(params, "user_id");
      if (!uid) uid = ccord_store_self_id(m->store);
      int64_t mask = 0; (void)ccord_json_int64(ccord_json_get(params, "perm_mask"), &mask);
      bool has = ccord_store_has_permission(m->store, gid, cid, uid, (uint64_t)mask);
      ccord_jw_bool(&w, has);
    } else if (strcmp(method, "store.voice_states") == 0) {
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      size_t n = ccord_store_voice_count(m->store, cid);
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < n; i++) {
        const ccord_voice_state *vs = ccord_store_voice_at(m->store, cid, i);
        if (!vs) continue;
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, vs->guild_id);
        ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, vs->channel_id);
        ccord_jw_key(&w, "user_id"); ccord_jw_id(&w, vs->user_id);
        ccord_jw_key(&w, "self_mute"); ccord_jw_bool(&w, vs->self_mute);
        ccord_jw_key(&w, "self_deaf"); ccord_jw_bool(&w, vs->self_deaf);
        ccord_jw_key(&w, "self_video"); ccord_jw_bool(&w, vs->self_video);
        ccord_jw_key(&w, "self_stream"); ccord_jw_bool(&w, vs->self_stream);
        ccord_jw_key(&w, "mute"); ccord_jw_bool(&w, vs->mute);
        ccord_jw_key(&w, "deaf"); ccord_jw_bool(&w, vs->deaf);
        ccord_jw_end_object(&w);
      }
      ccord_jw_end_array(&w);
    } else if (strcmp(method, "store.speaking") == 0) {
      ccord_id uid = pl_parse_id_param(params, "user_id");
      ccord_jw_bool(&w, ccord_store_user_speaking(m->store, uid));
    } else if (strcmp(method, "store.rtc_stats") == 0) {
      ccord_rtc_stats st = {0};
      ccord_store_rtc_stats(m->store, &st);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "state"); ccord_jw_int(&w, st.state);
      ccord_jw_key(&w, "ssrc"); ccord_jw_int(&w, (int64_t)st.ssrc);
      ccord_jw_key(&w, "ping_ms"); ccord_jw_int(&w, st.ping_ms);
      ccord_jw_key(&w, "jitter_depth"); ccord_jw_int(&w, (int64_t)st.jitter_depth);
      ccord_jw_key(&w, "packets_lost"); ccord_jw_int(&w, (int64_t)st.packets_lost);
      ccord_jw_key(&w, "bitrate_kbps"); ccord_jw_int(&w, (int64_t)st.bitrate_kbps);
      ccord_jw_key(&w, "dave_epoch"); ccord_jw_int(&w, (int64_t)st.dave_epoch);
      ccord_jw_end_object(&w);
    } else if (strcmp(method, "store.dave_status") == 0) {
      ccord_dave_status ds = {0};
      ccord_store_dave_status(m->store, &ds);
      ccord_jw_begin_object(&w);
      ccord_jw_key(&w, "active"); ccord_jw_bool(&w, ds.active);
      ccord_jw_key(&w, "verified"); ccord_jw_bool(&w, ds.verified);
      ccord_jw_key(&w, "protocol_version"); ccord_jw_int(&w, ds.protocol_version);
      ccord_jw_key(&w, "privacy_code"); ccord_jw_cstr(&w, ds.privacy_code);
      ccord_jw_end_object(&w);
    } else if (strcmp(method, "store.relationships") == 0) {
      size_t n = ccord_store_relationship_count(m->store);
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < n; i++) {
        const ccord_relationship *rel = ccord_store_relationship_at(m->store, i);
        if (!rel) continue;
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "user_id"); ccord_jw_id(&w, rel->user_id);
        ccord_jw_key(&w, "kind"); ccord_jw_int(&w, (int64_t)rel->kind);
        ccord_jw_end_object(&w);
      }
      ccord_jw_end_array(&w);
    } else if (strcmp(method, "store.user_profile") == 0) {
      ccord_id uid = pl_parse_id_param(params, "user_id");
      const ccord_user_profile *pr = ccord_store_user_profile(m->store, uid);
      if (!pr) ccord_jw_null(&w);
      else {
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "user_id"); ccord_jw_id(&w, pr->user_id);
        ccord_jw_key(&w, "display_name"); ccord_jw_cstr(&w, pr->display_name ? pr->display_name : "");
        ccord_jw_key(&w, "handle"); ccord_jw_cstr(&w, pr->handle ? pr->handle : "");
        ccord_jw_key(&w, "bio"); ccord_jw_cstr(&w, pr->bio ? pr->bio : "");
        ccord_jw_key(&w, "pronouns"); ccord_jw_cstr(&w, pr->pronouns ? pr->pronouns : "");
        ccord_jw_key(&w, "avatar_url"); ccord_jw_cstr(&w, pr->avatar_url ? pr->avatar_url : "");
        ccord_jw_key(&w, "banner_url"); ccord_jw_cstr(&w, pr->banner_url ? pr->banner_url : "");
        ccord_jw_key(&w, "accent_color"); ccord_jw_int(&w, (int64_t)pr->accent_color);
        ccord_jw_key(&w, "clan_tag"); ccord_jw_cstr(&w, pr->clan_tag);
        ccord_jw_end_object(&w);
      }
    } else if (strcmp(method, "store.user_note") == 0) {
      const char *note = ccord_store_user_note(m->store, pl_parse_id_param(params, "user_id"));
      ccord_jw_cstr(&w, note ? note : "");
    } else if (strcmp(method, "store.emojis") == 0) {
      ccord_id gid = pl_parse_id_param(params, "guild_id");
      size_t n = ccord_store_guild_emoji_count(m->store, gid);
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < n; i++) {
        const ccord_emoji *e = ccord_store_guild_emoji_at(m->store, gid, i);
        if (!e) continue;
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "id"); ccord_jw_id(&w, e->id);
        ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, e->name ? e->name : "");
        ccord_jw_key(&w, "animated"); ccord_jw_bool(&w, e->animated);
        ccord_jw_end_object(&w);
      }
      ccord_jw_end_array(&w);
    } else if (strcmp(method, "store.forum_posts") == 0) {
      ccord_id fid = pl_parse_id_param(params, "channel_id");
      size_t n = 0;
      const ccord_forum_post *ps = ccord_store_forum_posts(m->store, fid, &n);
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < n; i++) {
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "thread_id"); ccord_jw_id(&w, ps[i].thread_id);
        ccord_jw_key(&w, "title"); ccord_jw_cstr(&w, ps[i].title ? ps[i].title : "");
        ccord_jw_key(&w, "owner_id"); ccord_jw_id(&w, ps[i].owner_id);
        ccord_jw_key(&w, "message_count"); ccord_jw_int(&w, (int64_t)ps[i].message_count);
        ccord_jw_key(&w, "archived"); ccord_jw_bool(&w, ps[i].archived);
        ccord_jw_key(&w, "locked"); ccord_jw_bool(&w, ps[i].locked);
        ccord_jw_key(&w, "pinned"); ccord_jw_bool(&w, ps[i].pinned);
        ccord_jw_end_object(&w);
      }
      ccord_jw_end_array(&w);
    } else if (strcmp(method, "store.scheduled_events") == 0) {
      ccord_id gid = pl_parse_id_param(params, "guild_id");
      size_t n = ccord_store_scheduled_event_count(m->store, gid);
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < n; i++) {
        const ccord_scheduled_event *ev = ccord_store_scheduled_event_at(m->store, gid, i);
        if (!ev) continue;
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "id"); ccord_jw_id(&w, ev->id);
        ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ev->guild_id);
        ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, ev->channel_id);
        ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, ev->name);
        ccord_jw_key(&w, "description"); ccord_jw_cstr(&w, ev->description);
        ccord_jw_key(&w, "location"); ccord_jw_cstr(&w, ev->location);
        ccord_jw_key(&w, "start_ms"); ccord_jw_int(&w, ev->scheduled_start_ms);
        ccord_jw_key(&w, "end_ms"); ccord_jw_int(&w, ev->scheduled_end_ms);
        ccord_jw_key(&w, "status"); ccord_jw_int(&w, ev->status);
        ccord_jw_key(&w, "user_count"); ccord_jw_int(&w, (int64_t)ev->user_count);
        ccord_jw_key(&w, "me_rsvped"); ccord_jw_bool(&w, ev->me_rsvped);
        ccord_jw_end_object(&w);
      }
      ccord_jw_end_array(&w);
    } else if (strcmp(method, "store.typing") == 0) {
      ccord_id cid = pl_parse_id_param(params, "channel_id");
      size_t n = ccord_store_typing_count(m->store, cid);
      ccord_jw_begin_array(&w);
      for (size_t i = 0; i < n; i++) {
        ccord_jw_id(&w, ccord_store_typing_user_at(m->store, cid, i));
      }
      ccord_jw_end_array(&w);
    } else if (strcmp(method, "store.active_call") == 0) {
      ccord_id vch = ccord_store_active_voice_channel(m->store);
      const ccord_call_state *call = vch ? ccord_store_call(m->store, vch) : nullptr;
      if (!vch && !call) ccord_jw_null(&w);
      else {
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, vch ? vch : call->channel_id);
        ccord_jw_key(&w, "active"); ccord_jw_bool(&w, call ? call->active : (vch != 0));
        if (call) { ccord_jw_key(&w, "region"); ccord_jw_cstr(&w, call->region); }
        ccord_jw_end_object(&w);
      }
    } else {
      ccord_buf_free(&b);
      return false;
    }
    if (ccord_jw_ok(&w)) pl_reply_ok(m, p, id, ccord_buf_cstr(&b));
    else pl_reply_err(m, p, id, "internal", "store encode failed");
    ccord_buf_free(&b);
    return true;
  }
  return false;
}

bool ext_handle_presence_voice_social_window(ccord_plugins *m, plugin *p, uint32_t id, const char *method,
                                                const ccord_json *params)
{
  ccord_error err = {0};
  if (strcmp(method, "presence.set_activity") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_PRESENCE_WRITE)) { pl_fail_plugin(m, p, "undeclared restricted API called: presence.write"); return true; }
    const ccord_json *act = ccord_json_get(params, "activity");
    if (!act || ccord_json_type_of(act) != CCORD_JSON_OBJECT) {
      pl_reply_err(m, p, id, "invalid_argument", "activity object required");
      return true;
    }
    // Wrap single activity in JSON array for ccord_session_set_rich_presence.
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_array(&w);
    pl_jw_copy_json(&w, act);
    ccord_jw_end_array(&w);
    if (!m->session || !ccord_jw_ok(&w) || ccord_session_set_rich_presence(m->session, ccord_buf_cstr(&b), &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "set_activity failed");
    } else pl_reply_ok(m, p, id, nullptr);
    ccord_buf_free(&b);
    return true;
  }
  if (strcmp(method, "presence.clear_activity") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_PRESENCE_WRITE)) { pl_fail_plugin(m, p, "undeclared restricted API called: presence.write"); return true; }
    if (m->session) (void)ccord_session_set_rich_presence(m->session, nullptr, &err);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "presence.set_custom_status") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_PRESENCE_WRITE)) { pl_fail_plugin(m, p, "undeclared restricted API called: presence.write"); return true; }
    size_t el = 0, tl = 0;
    const char *emoji = pl_str_param(params, "emoji", &el);
    const char *text = pl_str_param(params, "text", &tl);
    ccord_id eid = pl_parse_id_param(params, "emoji_id");
    int64_t exp = 0; (void)ccord_json_int64(ccord_json_get(params, "expires_ms"), &exp);
    if (!m->session || ccord_session_set_custom_status(m->session, emoji ? emoji : "", eid, text ? text : "", exp, &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "internal", err.message[0] ? err.message : "set_custom_status failed");
    } else pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strncmp(method, "voice.", 6) == 0) {
    if (strcmp(method, "voice.levels") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_VOICE_LEVELS)) { pl_fail_plugin(m, p, "undeclared restricted API called: voice.levels"); return true; }
      char *j = m->plat_ops.voice_levels_json ? m->plat_ops.voice_levels_json(m->plat_ops.ud) : nullptr;
      pl_reply_ok(m, p, id, j ? j : "{\"mic\":0.0,\"output\":0.0,\"users\":{}}");
      ccord_free(j);
      return true;
    }
    if (strcmp(method, "voice.pcm.subscribe") == 0 || strcmp(method, "voice.pcm.unsubscribe") == 0 ||
        strcmp(method, "voice.pcm.push") == 0) {
      if (!pl_has_perm(p, CCORD_PERM_VOICE_PCM)) { pl_fail_plugin(m, p, "undeclared restricted API called: voice.pcm"); return true; }
      if (strcmp(method, "voice.pcm.subscribe") == 0) p->voice_pcm_sub = true;
      else if (strcmp(method, "voice.pcm.unsubscribe") == 0) p->voice_pcm_sub = false;
      else {
        size_t bl = 0; const char *b64 = pl_str_param(params, "b64", &bl);
        size_t cap = (bl / 4u) * 3u + 4u;
        uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
        size_t rlen = 0;
        if (raw && pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen) && rlen >= 2u) {
          size_t samples = rlen / 2u;
          int16_t *pcm = ccord_malloc(samples * sizeof(int16_t));
          if (pcm) {
            memcpy(pcm, raw, samples * 2u);
            if (m->plat_ops.voice_push_pcm) (void)m->plat_ops.voice_push_pcm(m->plat_ops.ud, pcm, samples);
            else if (m->session) (void)ccord_session_push_voice_pcm(m->session, pcm, samples, &err);
            ccord_free(pcm);
          }
        }
        ccord_free(raw);
      }
      pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (!pl_has_perm(p, CCORD_PERM_VOICE_CONTROL)) { pl_fail_plugin(m, p, "undeclared restricted API called: voice.control"); return true; }
    if (m->session) {
      if (strcmp(method, "voice.set_camera") == 0) {
        (void)ccord_session_set_voice_camera(m->session, ccord_json_bool(ccord_json_get(params, "enabled"), false), &err);
      } else if (strcmp(method, "voice.set_user_volume") == 0) {
        int64_t pct = 100; (void)ccord_json_int64(ccord_json_get(params, "pct"), &pct);
        (void)ccord_session_set_user_volume(m->session, pl_parse_id_param(params, "user_id"), (uint8_t)pct, &err);
      } else if (strcmp(method, "voice.set_user_local_mute") == 0) {
        // Local mute via 0% volume or store helper if mutable; route via set_user_volume(0) as fallback
        bool mut = ccord_json_bool(ccord_json_get(params, "muted"), false);
        (void)ccord_session_set_user_volume(m->session, pl_parse_id_param(params, "user_id"), mut ? 0u : 100u, &err);
      } else if (strcmp(method, "voice.play_soundboard") == 0) {
        (void)ccord_session_play_soundboard(m->session, pl_parse_id_param(params, "guild_id"), pl_parse_id_param(params, "sound_id"), &err);
      } else if (strcmp(method, "voice.call_join") == 0) {
        (void)ccord_session_call_join(m->session, pl_parse_id_param(params, "channel_id"), &err);
      } else if (strcmp(method, "voice.call_leave") == 0) {
        (void)ccord_session_call_leave(m->session, pl_parse_id_param(params, "channel_id"), &err);
      } else if (strcmp(method, "voice.watch_stream") == 0) {
        (void)ccord_session_watch_stream(m->session, pl_parse_id_param(params, "guild_id"),
                                         pl_parse_id_param(params, "channel_id"), pl_parse_id_param(params, "user_id"), &err);
      } else if (strcmp(method, "voice.stop_watching") == 0) {
        (void)ccord_session_stop_watching(m->session, &err);
      }
    }
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strncmp(method, "social.", 7) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_SOCIAL_WRITE)) { pl_fail_plugin(m, p, "undeclared restricted API called: social.write"); return true; }
    if (m->session) {
      if (strcmp(method, "social.open_dm") == 0) {
        (void)ccord_session_open_dm(m->session, pl_parse_id_param(params, "user_id"), &err);
      } else if (strcmp(method, "social.set_user_note") == 0) {
        ccord_id uid = pl_parse_id_param(params, "user_id");
        size_t nl = 0; const char *note = pl_str_param(params, "note", &nl);
        ccord_admin_request req = {0};
        if (ccord_admin_set_user_note(&req, uid, note ? note : "", &err) == CCORD_OK) {
          (void)ccord_session_admin(m->session, &req, &err);
        }
      } else if (strcmp(method, "social.open_thread") == 0) {
        size_t nl = 0; const char *nm = pl_str_param(params, "name", &nl);
        (void)ccord_session_open_thread(m->session, pl_parse_id_param(params, "guild_id"),
                                        pl_parse_id_param(params, "parent_id"), pl_parse_id_param(params, "thread_id"),
                                        nm ? nm : "", &err);
      } else if (strcmp(method, "social.create_forum_post") == 0) {
        size_t tl = 0, cl = 0;
        const char *title = pl_str_param(params, "title", &tl);
        const char *cnt = pl_str_param(params, "content", &cl);
        ccord_id tags[CCORD_FORUM_MAX_APPLIED_TAGS]; size_t nt = 0;
        const ccord_json *tj = ccord_json_get(params, "tags");
        if (tj && ccord_json_type_of(tj) == CCORD_JSON_ARRAY) {
          for (size_t i = 0; i < ccord_json_array_len(tj) && nt < CCORD_FORUM_MAX_APPLIED_TAGS; i++) {
            size_t sl = 0; const char *ts = ccord_json_string(ccord_json_array_at(tj, i), &sl);
            if (ts && sl > 0) tags[nt++] = (ccord_id)strtoull(ts, nullptr, 10);
          }
        }
        (void)ccord_session_create_forum_post(m->session, pl_parse_id_param(params, "forum_id"),
                                              title ? title : "", tl, cnt ? cnt : "", cl, tags, nt, &err);
      } else if (strcmp(method, "social.vote_poll") == 0) {
        uint32_t aids[CCORD_RICH_MAX_POLL_ANSWERS]; size_t na = 0;
        const ccord_json *aj = ccord_json_get(params, "answer_ids");
        if (aj && ccord_json_type_of(aj) == CCORD_JSON_ARRAY) {
          for (size_t i = 0; i < ccord_json_array_len(aj) && na < CCORD_RICH_MAX_POLL_ANSWERS; i++) {
            int64_t v = 0;
            if (ccord_json_int64(ccord_json_array_at(aj, i), &v) && v > 0) aids[na++] = (uint32_t)v;
          }
        }
        (void)ccord_session_vote_poll(m->session, pl_parse_id_param(params, "channel_id"),
                                      pl_parse_id_param(params, "message_id"), aids, na, &err);
      } else if (strcmp(method, "social.rsvp_event") == 0) {
        (void)ccord_session_rsvp_scheduled_event(m->session, pl_parse_id_param(params, "guild_id"),
                                                 pl_parse_id_param(params, "event_id"),
                                                 ccord_json_bool(ccord_json_get(params, "rsvp"), true), &err);
      }
    }
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "window.is_focused") == 0) {
    pl_reply_ok(m, p, id, ccord_contrib_ui_window_is_focused(m->contrib) ? "true" : "false");
    return true;
  }
  if (strncmp(method, "window.", 7) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_UI_WINDOW)) { pl_fail_plugin(m, p, "undeclared restricted API called: ui.window"); return true; }
    if (strcmp(method, "window.set_title") == 0) {
      size_t tl = 0; const char *t = pl_str_param(params, "title", &tl);
      if (t) (void)ccord_contrib_ui_window_set_title(m->contrib, t);
    } else if (strcmp(method, "window.set_badge") == 0) {
      int64_t cnt = 0; (void)ccord_json_int64(ccord_json_get(params, "count"), &cnt);
      (void)ccord_contrib_ui_window_set_badge(m->contrib, (int)cnt);
    } else if (strcmp(method, "window.flash") == 0) {
      (void)ccord_contrib_ui_window_flash(m->contrib);
    }
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- Public API v3 client -> plugin dispatchers

bool ccord_plugins_has_raw_gateway_subscribers(const ccord_plugins *m)
{
  if (!m) return false;
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = m->plugins[i];
    if (!p->used || p->state != CCORD_PLUGIN_READY || !pl_has_perm(p, CCORD_PERM_GATEWAY_RAW)) continue;
    if (p->gw_raw_any || p->ngw_raw_events > 0) return true;
  }
  return false;
}

void ccord_plugins_dispatch_raw_gateway(ccord_plugins *m, const char *event_name, const char *payload_json)
{
  if (!m || !event_name || !event_name[0]) return;
  bool any = false;
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = m->plugins[i];
    if (!p->used || p->state != CCORD_PLUGIN_READY || !pl_has_perm(p, CCORD_PERM_GATEWAY_RAW)) continue;
    if (p->gw_raw_any) { any = true; break; }
    for (size_t k = 0; k < p->ngw_raw_events; k++) {
      if (strcmp(p->gw_raw_events[k], event_name) == 0) { any = true; break; }
    }
    if (any) break;
  }
  if (!any) return;
  ccord_buf b; ccord_buf_init(&b, 0);
  ccord_jw w; ccord_jw_init(&w, &b);
  ccord_arena ar; ccord_arena_init(&ar, 0);
  const ccord_json *pj = nullptr;
  if (payload_json && payload_json[0]) {
    ccord_error pe = {0};
    pj = ccord_json_parse(&ar, payload_json, strlen(payload_json), &k_ext_limits, &pe);
  }
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "event"); ccord_jw_cstr(&w, event_name);
  ccord_jw_key(&w, "payload");
  if (pj) pl_jw_copy_json(&w, pj); else ccord_jw_null(&w);
  ccord_jw_end_object(&w);
  ccord_arena_free(&ar);
  if (ccord_jw_ok(&w)) {
    for (size_t i = 0; i < m->nplugins; i++) {
      plugin *p = m->plugins[i];
      if (!p->used || p->state != CCORD_PLUGIN_READY || !p->ipc || !pl_has_perm(p, CCORD_PERM_GATEWAY_RAW)) continue;
      bool want = p->gw_raw_any;
      for (size_t k = 0; !want && k < p->ngw_raw_events; k++) {
        if (strcmp(p->gw_raw_events[k], event_name) == 0) want = true;
      }
      if (want) pl_send_ntf(m, p, "gateway.raw", ccord_buf_cstr(&b));
    }
  }
  ccord_buf_free(&b);
}

void ccord_plugins_dispatch_theme_change(ccord_plugins *m, const char *theme_id)
{
  if (!m || !theme_id) return;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w); ccord_jw_key(&w, "theme_id"); ccord_jw_cstr(&w, theme_id); ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_broadcast_event_json(m, EVB_THEME_CHANGE, "theme.change", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

void ccord_plugins_dispatch_window_focus(ccord_plugins *m, bool focused)
{
  if (!m) return;
  pl_broadcast_event_json(m, EVB_WINDOW_FOCUS, "window.focus", focused ? "{\"focused\":true}" : "{\"focused\":false}");
}

void ccord_plugins_dispatch_window_resize(ccord_plugins *m, float width, float height)
{
  if (!m) return;
  char dj[96];
  snprintf(dj, sizeof dj, "{\"width\":%.1f,\"height\":%.1f}", (double)width, (double)height);
  pl_broadcast_event_json(m, EVB_WINDOW_RESIZE, "window.resize", dj);
}

void ccord_plugins_dispatch_streamer_mode(ccord_plugins *m, bool enabled)
{
  if (!m) return;
  pl_broadcast_event_json(m, EVB_STREAMER_MODE, "streamer_mode.change", enabled ? "{\"enabled\":true}" : "{\"enabled\":false}");
}

void ccord_plugins_dispatch_composer_change(ccord_plugins *m, ccord_id channel_id, const char *text)
{
  if (!m) return;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, channel_id);
  ccord_jw_key(&w, "text"); ccord_jw_cstr(&w, text ? text : "");
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_broadcast_event_json(m, EVB_COMPOSER_CHANGE, "composer.change", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

void ccord_plugins_dispatch_voice_pcm(ccord_plugins *m, const int16_t *pcm, size_t samples)
{
  if (!m || !pcm || samples == 0) return;
  bool any = false;
  for (size_t i = 0; i < m->nplugins; i++) {
    const plugin *p = m->plugins[i];
    if (p->used && p->state == CCORD_PLUGIN_READY && p->voice_pcm_sub && pl_has_perm(p, CCORD_PERM_VOICE_PCM)) {
      any = true;
      break;
    }
  }
  if (!any) return;
  size_t bytes = samples * 2u;
  ccord_buf b64; ccord_buf_init(&b64, (bytes * 4u) / 3u + 8u);
  pl_fs_b64_encode((const unsigned char *)pcm, bytes, &b64);
  ccord_buf out; ccord_buf_init(&out, 0); ccord_jw w; ccord_jw_init(&w, &out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "b64"); ccord_jw_string(&w, b64.data ? b64.data : "", b64.len);
  ccord_jw_key(&w, "samples"); ccord_jw_int(&w, (int64_t)samples);
  ccord_jw_end_object(&w);
  ccord_buf_free(&b64);
  if (ccord_jw_ok(&w)) {
    for (size_t i = 0; i < m->nplugins; i++) {
      plugin *p = m->plugins[i];
      if (p->used && p->state == CCORD_PLUGIN_READY && p->ipc && p->voice_pcm_sub && pl_has_perm(p, CCORD_PERM_VOICE_PCM)) {
        pl_send_ntf(m, p, "voice.pcm", ccord_buf_cstr(&out));
      }
    }
  }
  ccord_buf_free(&out);
}

void ccord_plugins_dispatch_voice_levels(ccord_plugins *m, const char *levels_json)
{
  if (!m || !levels_json) return;
  pl_broadcast_event_json(m, EVB_VOICE_LEVELS, "voice.levels", levels_json);
}
