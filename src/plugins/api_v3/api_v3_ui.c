// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _GNU_SOURCE
#include "api_v3_internal.h"

// ---------------------------------------------------------------- Theme layer upsert helper

static void upsert_theme_layer_field(ccord_plugins *m, plugin *p, const char *layer_id,
                                     const char *base_id, const char *css, const char *colors_json,
                                     const char *metrics_json, const char *font_reg, const char *font_bold,
                                     const char *bg_slot, const char *bg_tex, float bg_opacity, const char *bg_fit)
{
  ccord_theme_layer merged = {
    .owner = p->m.id,
    .layer_id = layer_id && layer_id[0] ? layer_id : "default",
    .base_id = "",
    .css = "",
    .colors_json = "",
    .metrics_json = "",
    .font_regular = "",
    .font_bold = "",
    .bg_slot = "",
    .bg_texture_key = "",
    .bg_opacity = 1.0f,
    .bg_fit = "cover",
    .priority = 0,
  };
  // Preserve existing fields on the same (owner, layer_id) layer.
  size_t nl = ccord_contrib_theme_layer_count(m->contrib);
  for (size_t i = 0; i < nl; i++) {
    const ccord_theme_layer *cur = ccord_contrib_theme_layer_at(m->contrib, i);
    if (cur && strcmp(cur->owner, p->m.id) == 0 && strcmp(cur->layer_id, merged.layer_id) == 0) {
      merged = *cur;
      break;
    }
  }
  if (base_id) merged.base_id = base_id;
  if (css) merged.css = css;
  if (colors_json) merged.colors_json = colors_json;
  if (metrics_json) merged.metrics_json = metrics_json;
  if (font_reg) merged.font_regular = font_reg;
  if (font_bold) merged.font_bold = font_bold;
  if (bg_slot) {
    merged.bg_slot = bg_slot;
    merged.bg_texture_key = bg_tex ? bg_tex : "";
    merged.bg_opacity = bg_opacity;
    merged.bg_fit = bg_fit && bg_fit[0] ? bg_fit : "cover";
  }
  ccord_error err = {0};
  (void)ccord_contrib_set_theme_layer(m->contrib, &merged, &err);
}

// ---------------------------------------------------------------- Request handlers (Class 1 / 2 / 3)

bool ext_handle_settings_and_theme(ccord_plugins *m, plugin *p, uint32_t id, const char *method,
                                      const ccord_json *params)
{
  if (strcmp(method, "settings.schema") == 0) {
    const ccord_json *sj = params ? ccord_json_get(params, "schema") : nullptr;
    if (!sj || ccord_json_type_of(sj) != CCORD_JSON_ARRAY) {
      pl_reply_err(m, p, id, "invalid_argument", "settings.schema: array required");
      return true;
    }
    ccord_buf sb, vb; ccord_buf_init(&sb, 0); ccord_buf_init(&vb, 0);
    ext_encode_json_str(sj, &sb);
    // Build values snapshot from p->items
    ccord_jw vw; ccord_jw_init(&vw, &vb);
    ccord_jw_begin_object(&vw);
    ccord_arena ar; ccord_arena_init(&ar, 0);
    for (size_t i = 0; i < p->nitems; i++) {
      ccord_arena_reset(&ar);
      ccord_error pe = {0};
      const ccord_json *val = ccord_json_parse(&ar, p->items[i].json, p->items[i].json_len, &k_ext_limits, &pe);
      ccord_jw_key(&vw, p->items[i].key);
      if (val) pl_jw_copy_json(&vw, val); else ccord_jw_null(&vw);
    }
    ccord_arena_free(&ar);
    ccord_jw_end_object(&vw);
    ccord_error err = {0};
    ccord_status st = ccord_contrib_set_settings_schema(m->contrib, p->m.id, ccord_buf_cstr(&sb), ccord_buf_cstr(&vb), &err);
    ccord_buf_free(&sb); ccord_buf_free(&vb);
    if (st != CCORD_OK) pl_reply_err(m, p, id, "invalid_argument", err.message);
    else pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "theme.current") == 0) {
    char *j = m->plat_ops.theme_current_json ? m->plat_ops.theme_current_json(m->plat_ops.ud) : nullptr;
    pl_reply_ok(m, p, id, j ? j : "{\"id\":\"c-cord-dark\",\"name\":\"Dark\",\"colors\":{},\"metrics\":{}}");
    ccord_free(j);
    return true;
  }
  if (strcmp(method, "theme.color") == 0) {
    size_t tl = 0; const char *tok = pl_str_param(params, "token", &tl);
    char *hex = (tok && m->plat_ops.theme_color_hex) ? m->plat_ops.theme_color_hex(m->plat_ops.ud, tok) : nullptr;
    if (hex) {
      char buf[64];
      snprintf(buf, sizeof buf, "\"%s\"", hex);
      pl_reply_ok(m, p, id, buf);
      ccord_free(hex);
    } else {
      pl_reply_ok(m, p, id, "null");
    }
    return true;
  }
  if (strcmp(method, "theme.metric") == 0) {
    size_t ml = 0; const char *met = pl_str_param(params, "metric", &ml);
    double val = 0.0;
    if (met && m->plat_ops.theme_metric_val && m->plat_ops.theme_metric_val(m->plat_ops.ud, met, &val)) {
      char buf[64];
      snprintf(buf, sizeof buf, "%.6g", val);
      pl_reply_ok(m, p, id, buf);
    } else {
      pl_reply_ok(m, p, id, "null");
    }
    return true;
  }
  if (strcmp(method, "theme.list") == 0) {
    char *j = m->plat_ops.theme_list_json ? m->plat_ops.theme_list_json(m->plat_ops.ud) : nullptr;
    if (j) {
      pl_reply_ok(m, p, id, j);
      ccord_free(j);
    } else {
      ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
      ccord_jw_begin_array(&w);
      static const char *const k_builtin[][2] = {{"c-cord-dark", "Dark"}, {"c-cord-light", "Light"}, {"c-cord-amoled", "AMOLED"}};
      for (size_t i = 0; i < 3; i++) {
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, k_builtin[i][0]);
        ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, k_builtin[i][1]);
        ccord_jw_key(&w, "builtin"); ccord_jw_bool(&w, true);
        ccord_jw_end_object(&w);
      }
      for (size_t i = 0; i < ccord_contrib_theme_preset_count(m->contrib); i++) {
        const ccord_theme_preset *pr = ccord_contrib_theme_preset_at(m->contrib, i);
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, pr->preset_id);
        ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, pr->name);
        ccord_jw_key(&w, "owner"); ccord_jw_cstr(&w, pr->owner);
        ccord_jw_key(&w, "builtin"); ccord_jw_bool(&w, false);
        ccord_jw_end_object(&w);
      }
      ccord_jw_end_array(&w);
      pl_reply_ok(m, p, id, ccord_jw_ok(&w) ? ccord_buf_cstr(&b) : "[]");
      ccord_buf_free(&b);
    }
    return true;
  }
  if (strcmp(method, "theme.register") == 0) {
    size_t il = 0, nl = 0, bl = 0, cl = 0;
    const char *pid = pl_str_param(params, "id", &il);
    const char *name = pl_str_param(params, "name", &nl);
    const char *base = pl_str_param(params, "base", &bl);
    const char *css = pl_str_param(params, "css", &cl);
    if (!pid || !pl_valid_menu_id(pid, il)) { pl_reply_err(m, p, id, "invalid_argument", "theme.register: invalid id"); return true; }
    if (!name || !pl_valid_plain_text(name, nl, 64)) { pl_reply_err(m, p, id, "invalid_argument", "theme.register: invalid name"); return true; }
    char freg[CCORD_PATH_MAX] = "", fbold[CCORD_PATH_MAX] = "";
    const ccord_json *fj = ccord_json_get(params, "fonts");
    if (fj && ccord_json_type_of(fj) == CCORD_JSON_OBJECT) {
      size_t rl = 0, bdl = 0;
      const char *rr = pl_str_param(fj, "regular", &rl);
      const char *rb = pl_str_param(fj, "bold", &bdl);
      if (rr && rr[0] && !ext_resolve_plugin_asset_path(p, rr, freg, sizeof freg)) {
        pl_reply_err(m, p, id, "invalid_argument", "theme.register: invalid regular font path");
        return true;
      }
      if (rb && rb[0] && !ext_resolve_plugin_asset_path(p, rb, fbold, sizeof fbold)) {
        pl_reply_err(m, p, id, "invalid_argument", "theme.register: invalid bold font path");
        return true;
      }
    }
    ccord_buf col_b, met_b; ccord_buf_init(&col_b, 0); ccord_buf_init(&met_b, 0);
    if (ccord_json_get(params, "colors")) ext_encode_json_str(ccord_json_get(params, "colors"), &col_b);
    if (ccord_json_get(params, "metrics")) ext_encode_json_str(ccord_json_get(params, "metrics"), &met_b);
    ccord_theme_preset pr = {
      .owner = p->m.id, .preset_id = pid, .name = name,
      .base_id = (base && base[0]) ? base : "c-cord-dark",
      .css = css ? css : "",
      .colors_json = col_b.len ? ccord_buf_cstr(&col_b) : "",
      .metrics_json = met_b.len ? ccord_buf_cstr(&met_b) : "",
      .font_regular = freg, .font_bold = fbold,
    };
    ccord_error err = {0};
    ccord_status st = ccord_contrib_add_theme_preset(m->contrib, &pr, &err);
    ccord_buf_free(&col_b); ccord_buf_free(&met_b);
    if (st != CCORD_OK) pl_reply_err(m, p, id, "invalid_argument", err.message);
    else pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "theme.unregister") == 0) {
    size_t il = 0; const char *pid = pl_str_param(params, "id", &il);
    if (pid) (void)ccord_contrib_remove_theme_preset(m->contrib, p->m.id, pid);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strncmp(method, "theme.", 6) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_THEME_WRITE)) {
      pl_fail_plugin(m, p, "undeclared restricted API called: theme.write");
      return true;
    }
    if (strcmp(method, "theme.set_base") == 0) {
      size_t bl = 0; const char *base = pl_str_param(params, "base_id", &bl);
      upsert_theme_layer_field(m, p, "default", base ? base : "c-cord-dark", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 1.0f, nullptr);
    } else if (strcmp(method, "theme.patch_colors") == 0) {
      size_t ll = 0; const char *lid = pl_str_param(params, "layer_id", &ll);
      ccord_buf cb; ccord_buf_init(&cb, 0);
      if (ccord_json_get(params, "colors")) ext_encode_json_str(ccord_json_get(params, "colors"), &cb);
      upsert_theme_layer_field(m, p, lid, nullptr, nullptr, ccord_buf_cstr(&cb), nullptr, nullptr, nullptr, nullptr, nullptr, 1.0f, nullptr);
      ccord_buf_free(&cb);
    } else if (strcmp(method, "theme.patch_metrics") == 0) {
      size_t ll = 0; const char *lid = pl_str_param(params, "layer_id", &ll);
      ccord_buf mb; ccord_buf_init(&mb, 0);
      if (ccord_json_get(params, "metrics")) ext_encode_json_str(ccord_json_get(params, "metrics"), &mb);
      upsert_theme_layer_field(m, p, lid, nullptr, nullptr, nullptr, ccord_buf_cstr(&mb), nullptr, nullptr, nullptr, nullptr, 1.0f, nullptr);
      ccord_buf_free(&mb);
    } else if (strcmp(method, "theme.inject_css") == 0) {
      size_t ll = 0, cl = 0;
      const char *lid = pl_str_param(params, "layer_id", &ll);
      const char *css = pl_str_param(params, "css", &cl);
      upsert_theme_layer_field(m, p, lid, nullptr, css ? css : "", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 1.0f, nullptr);
    } else if (strcmp(method, "theme.remove_css") == 0) {
      size_t ll = 0; const char *lid = pl_str_param(params, "layer_id", &ll);
      if (lid) (void)ccord_contrib_remove_theme_layer(m->contrib, p->m.id, lid);
    } else if (strcmp(method, "theme.set_fonts") == 0) {
      size_t rl = 0, bl = 0;
      const char *rr = pl_str_param(params, "regular", &rl);
      const char *rb = pl_str_param(params, "bold", &bl);
      char freg[CCORD_PATH_MAX] = "", fbold[CCORD_PATH_MAX] = "";
      if (rr && rr[0] && !ext_resolve_plugin_asset_path(p, rr, freg, sizeof freg)) {
        pl_reply_err(m, p, id, "invalid_argument", "theme.set_fonts: regular font outside plugin dir");
        return true;
      }
      if (rb && rb[0] && !ext_resolve_plugin_asset_path(p, rb, fbold, sizeof fbold)) {
        pl_reply_err(m, p, id, "invalid_argument", "theme.set_fonts: bold font outside plugin dir");
        return true;
      }
      upsert_theme_layer_field(m, p, "default", nullptr, nullptr, nullptr, nullptr, freg, fbold, nullptr, nullptr, 1.0f, nullptr);
    } else if (strcmp(method, "theme.set_icon") == 0) {
      size_t il = 0, tl = 0;
      const char *icon = pl_str_param(params, "icon", &il);
      const char *tex = pl_str_param(params, "texture_key", &tl);
      ccord_error err = {0};
      if (icon && icon[0]) {
        if (tex && tex[0]) (void)ccord_contrib_set_icon_override(m->contrib, p->m.id, icon, tex, &err);
        else (void)ccord_contrib_remove_icon_override(m->contrib, p->m.id, icon);
      }
    } else if (strcmp(method, "theme.set_background") == 0) {
      size_t sl = 0; const char *slot = pl_str_param(params, "slot", &sl);
      const ccord_json *opts = ccord_json_get(params, "opts");
      size_t tl = 0, fl = 0;
      const char *tex = opts ? pl_str_param(opts, "texture", &tl) : nullptr;
      if (!tex && opts) tex = pl_str_param(opts, "texture_key", &tl);
      const char *fit = opts ? pl_str_param(opts, "fit", &fl) : "cover";
      double op = opts && ccord_json_get(opts, "opacity") ? ccord_json_number(ccord_json_get(opts, "opacity"), 0.25) : 0.25;
      char lid[48];
      snprintf(lid, sizeof lid, "bg_%s", slot ? slot : "chat");
      if (!opts || !tex || !tex[0]) {
        (void)ccord_contrib_remove_theme_layer(m->contrib, p->m.id, lid);
      } else {
        upsert_theme_layer_field(m, p, lid, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 slot ? slot : "chat", tex, (float)op, fit ? fit : "cover");
      }
    } else if (strcmp(method, "theme.reset") == 0) {
      ccord_contrib_clear_theme_layers(m->contrib, p->m.id);
    } else {
      pl_reply_err(m, p, id, "unsupported", "unknown theme method");
      return true;
    }
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  return false;
}

bool ext_handle_ui_v3(ccord_plugins *m, plugin *p, uint32_t id, const char *method, const ccord_json *params)
{
  if (strcmp(method, "keybinds.register") == 0) {
    size_t il = 0, ll = 0, dl = 0;
    const char *bid = pl_str_param(params, "id", &il);
    const char *lbl = pl_str_param(params, "label", &ll);
    const char *def = pl_str_param(params, "default", &dl);
    bool is_global = ccord_json_bool(ccord_json_get(params, "global"), false);
    if (is_global && !pl_has_perm(p, CCORD_PERM_SYSTEM_SHORTCUTS)) {
      pl_fail_plugin(m, p, "undeclared restricted API called: system.shortcuts");
      return true;
    }
    if (!bid || !pl_valid_menu_id(bid, il)) { pl_reply_err(m, p, id, "invalid_argument", "invalid keybind id"); return true; }
    ccord_keybind_contribution kb = {
      .owner = p->m.id, .bind_id = bid, .label = lbl ? lbl : bid,
      .default_combo = def ? def : "", .current_combo = def ? def : "", .is_global = is_global,
    };
    ccord_error err = {0};
    if (ccord_contrib_add_keybind(m->contrib, &kb, &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "invalid_argument", err.message);
      return true;
    }
    if (is_global && m->plat_ops.global_shortcuts_sync) m->plat_ops.global_shortcuts_sync(m->plat_ops.ud);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "keybinds.unregister") == 0) {
    size_t il = 0; const char *bid = pl_str_param(params, "id", &il);
    if (bid) (void)ccord_contrib_remove_keybind(m->contrib, p->m.id, bid);
    if (m->plat_ops.global_shortcuts_sync) m->plat_ops.global_shortcuts_sync(m->plat_ops.ud);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "ui.actionButton.add") == 0) {
    size_t il = 0, sl = 0, ll = 0, icl = 0, ttl = 0, bgl = 0;
    const char *bid = pl_str_param(params, "id", &il);
    const char *slot_s = pl_str_param(params, "slot", &sl);
    ccord_action_btn_slot slot = CCORD_ACTBTN_HEADER;
    if (slot_s) {
      if (strcmp(slot_s, "header") == 0) slot = CCORD_ACTBTN_HEADER;
      else if (strcmp(slot_s, "composer_plus") == 0) slot = CCORD_ACTBTN_COMPOSER_PLUS;
      else if (strcmp(slot_s, "message_hover") == 0) slot = CCORD_ACTBTN_MESSAGE_HOVER;
      else if (strcmp(slot_s, "user_panel") == 0) slot = CCORD_ACTBTN_USER_PANEL;
      else if (strcmp(slot_s, "tray") == 0) {
        if (!pl_has_perm(p, CCORD_PERM_UI_WINDOW)) {
          pl_fail_plugin(m, p, "undeclared restricted API called: ui.window");
          return true;
        }
        slot = CCORD_ACTBTN_TRAY;
      } else {
        pl_reply_err(m, p, id, "invalid_argument", "unknown actionButton slot");
        return true;
      }
    }
    ccord_action_btn_contribution btn = {
      .owner = p->m.id, .btn_id = bid ? bid : "", .slot = slot,
      .label = pl_str_param(params, "label", &ll),
      .icon = pl_str_param(params, "icon", &icl),
      .tooltip = pl_str_param(params, "tooltip", &ttl),
      .badge = pl_str_param(params, "badge", &bgl),
    };
    ccord_error err = {0};
    if (ccord_contrib_add_action_btn(m->contrib, &btn, &err) != CCORD_OK) {
      pl_reply_err(m, p, id, "invalid_argument", err.message);
    } else {
      pl_reply_ok(m, p, id, nullptr);
    }
    return true;
  }
  if (strcmp(method, "ui.actionButton.remove") == 0) {
    size_t il = 0; const char *bid = pl_str_param(params, "id", &il);
    if (bid) (void)ccord_contrib_remove_action_btn(m->contrib, p->m.id, bid);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "ui.dialog.open") == 0) {
    int64_t req_id = 0;
    (void)ccord_json_int64(ccord_json_get(params, "req_id"), &req_id);
    size_t kl = 0; const char *ks = pl_str_param(params, "kind", &kl);
    const ccord_json *opts = ccord_json_get(params, "opts");
    ccord_dialog_kind kind = CCORD_DIALOG_CONFIRM;
    if (ks && strcmp(ks, "prompt") == 0) kind = CCORD_DIALOG_PROMPT;
    else if (ks && strcmp(ks, "form") == 0) kind = CCORD_DIALOG_FORM;
    ccord_buf fields_b; ccord_buf_init(&fields_b, 0);
    if (opts && ccord_json_get(opts, "fields")) ext_encode_json_str(ccord_json_get(opts, "fields"), &fields_b);
    size_t tl = 0, ml = 0, cl = 0, xcl = 0, dl = 0;
    ccord_plugin_dialog dlg = {
      .owner = p->m.id, .plugin_name = p->m.name, .req_id = (uint64_t)req_id, .kind = kind,
      .title = opts ? pl_str_param(opts, "title", &tl) : nullptr,
      .message = opts ? pl_str_param(opts, "message", &ml) : nullptr,
      .confirm_label = opts ? pl_str_param(opts, "confirm_label", &cl) : nullptr,
      .cancel_label = opts ? pl_str_param(opts, "cancel_label", &xcl) : nullptr,
      .default_value = opts ? pl_str_param(opts, "default", &dl) : nullptr,
      .fields_json = fields_b.len ? ccord_buf_cstr(&fields_b) : "",
    };
    ccord_error err = {0};
    ccord_status st = ccord_contrib_open_dialog(m->contrib, &dlg, &err);
    ccord_buf_free(&fields_b);
    if (st != CCORD_OK) {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, err.message);
    }
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "ui.file.pick") == 0) {
    int64_t req_id = 0; (void)ccord_json_int64(ccord_json_get(params, "req_id"), &req_id);
    size_t tl = 0; const char *title = pl_str_param(params, "title", &tl);
    bool mult = ccord_json_bool(ccord_json_get(params, "multiple"), false);
    ccord_buf fb; ccord_buf_init(&fb, 0);
    if (ccord_json_get(params, "filters")) ext_encode_json_str(ccord_json_get(params, "filters"), &fb);
    char paths[4096] = "";
    bool ok = ccord_contrib_ui_file_pick(m->contrib, title ? title : "Select file",
                                         fb.len ? ccord_buf_cstr(&fb) : "", mult, paths, sizeof paths);
    ccord_buf_free(&fb);
    pl_reply_ok(m, p, id, nullptr);
    ext_send_async_reply(m, p, (uint64_t)req_id, ok, ok ? paths : nullptr, ok ? nullptr : "file picker cancelled or unavailable");
    return true;
  }
  if (strcmp(method, "ui.file.save") == 0) {
    int64_t req_id = 0; (void)ccord_json_int64(ccord_json_get(params, "req_id"), &req_id);
    size_t nl = 0, bl = 0;
    const char *sname = pl_str_param(params, "suggested_name", &nl);
    const char *b64 = pl_str_param(params, "b64", &bl);
    size_t cap = (bl / 4u) * 3u + 4u;
    uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
    size_t rlen = 0;
    bool dec_ok = !bl || (raw && pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen));
    char saved[CCORD_PATH_MAX] = "";
    bool ok = dec_ok && ccord_contrib_ui_file_save(m->contrib, sname ? sname : "export.txt", raw, rlen, saved, sizeof saved);
    ccord_free(raw);
    pl_reply_ok(m, p, id, nullptr);
    if (ok) {
      ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
      ccord_jw_begin_object(&w); ccord_jw_key(&w, "path"); ccord_jw_cstr(&w, saved); ccord_jw_end_object(&w);
      ext_send_async_reply(m, p, (uint64_t)req_id, true, ccord_buf_cstr(&b), nullptr);
      ccord_buf_free(&b);
    } else {
      ext_send_async_reply(m, p, (uint64_t)req_id, false, nullptr, "file save cancelled or unavailable");
    }
    return true;
  }
  if (strcmp(method, "ui.measure_text") == 0) {
    size_t tl = 0; const char *txt = pl_str_param(params, "text", &tl);
    float sz = (float)ccord_json_number(ccord_json_get(params, "size"), 14.0);
    bool bold = ccord_json_bool(ccord_json_get(params, "bold"), false);
    float wrap = (float)ccord_json_number(ccord_json_get(params, "wrap_width"), 0.0);
    float ow = 0.0f, oh = 0.0f;
    if (!ccord_contrib_ui_measure_text(m->contrib, txt ? txt : "", sz, bold, wrap, &ow, &oh)) {
      ow = (float)tl * (sz * 0.56f);
      oh = sz * 1.35f;
    }
    char buf[96];
    snprintf(buf, sizeof buf, "{\"width\":%.2f,\"height\":%.2f}", (double)ow, (double)oh);
    pl_reply_ok(m, p, id, buf);
    return true;
  }
  if (strcmp(method, "commands.unregister") == 0) {
    size_t nl = 0; const char *name = pl_str_param(params, "name", &nl);
    if (name) (void)ccord_contrib_remove_command(m->contrib, p->m.id, name);
    pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strcmp(method, "ui.panel.patch") == 0) {
    if (!pl_has_perm(p, CCORD_PERM_UI_PANEL)) { pl_fail_plugin(m, p, "undeclared restricted API called: ui.panel"); return true; }
    size_t il = 0; const char *pid = pl_str_param(params, "id", &il);
    const ccord_json *ops = ccord_json_get(params, "ops");
    if (!pid || !ops) { pl_reply_err(m, p, id, "invalid_argument", "ui.panel.patch requires id and ops"); return true; }
    ccord_buf ob; ccord_buf_init(&ob, 0);
    ext_encode_json_str(ops, &ob);
    ccord_error err = {0};
    ccord_status st = ccord_contrib_patch_panel(m->contrib, p->m.id, pid, ccord_buf_cstr(&ob), &err);
    ccord_buf_free(&ob);
    if (st != CCORD_OK) pl_reply_err(m, p, id, "invalid_argument", err.message);
    else pl_reply_ok(m, p, id, nullptr);
    return true;
  }
  if (strncmp(method, "ui.texture.", 11) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_UI_PANEL)) { pl_fail_plugin(m, p, "undeclared restricted API called: ui.panel"); return true; }
    size_t kl = 0; const char *key = pl_str_param(params, "key", &kl);
    if (strcmp(method, "ui.texture.upload") == 0) {
      size_t bl = 0; const char *b64 = pl_str_param(params, "b64", &bl);
      bool is_rgba = ccord_json_bool(ccord_json_get(params, "rgba"), false);
      int64_t w_px = 0, h_px = 0;
      (void)ccord_json_int64(ccord_json_get(params, "width"), &w_px);
      (void)ccord_json_int64(ccord_json_get(params, "height"), &h_px);
      if (!key || !b64 || bl == 0) { pl_reply_err(m, p, id, "invalid_argument", "missing key or b64"); return true; }
      size_t cap = (bl / 4u) * 3u + 4u;
      uint8_t *raw = ccord_malloc(cap);
      size_t rlen = 0;
      if (!raw || !pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen)) {
        ccord_free(raw);
        pl_reply_err(m, p, id, "invalid_argument", "invalid base64 texture");
        return true;
      }
      ccord_error err = {0};
      ccord_status st = ccord_contrib_set_texture(m->contrib, p->m.id, key, raw, rlen, (int)w_px, (int)h_px, is_rgba, &err);
      ccord_free(raw);
      if (st != CCORD_OK) pl_reply_err(m, p, id, "invalid_argument", err.message);
      else pl_reply_ok(m, p, id, nullptr);
    } else if (strcmp(method, "ui.texture.evict") == 0) {
      if (key) (void)ccord_contrib_remove_texture(m->contrib, p->m.id, key);
      pl_reply_ok(m, p, id, nullptr);
    } else if (strcmp(method, "ui.texture.exists") == 0) {
      bool ex = key && ccord_contrib_find_texture(m->contrib, key) != nullptr;
      pl_reply_ok(m, p, id, ex ? "true" : "false");
    } else {
      pl_reply_err(m, p, id, "unsupported", "unknown texture method");
    }
    return true;
  }
  if (strncmp(method, "ui.decorate.", 12) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_UI_DECORATE)) { pl_fail_plugin(m, p, "undeclared restricted API called: ui.decorate"); return true; }
    if (strcmp(method, "ui.decorate.clear") == 0) {
      ccord_contrib_clear_decorators(m->contrib, p->m.id);
      pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "ui.decorate.remove") == 0) {
      size_t il = 0; const char *did = pl_str_param(params, "id", &il);
      if (did) (void)ccord_contrib_remove_decorator(m->contrib, p->m.id, did);
      pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    if (strcmp(method, "ui.decorate.set") == 0) {
      size_t tl = 0; const char *ts = pl_str_param(params, "target", &tl);
      const ccord_json *sp = ccord_json_get(params, "spec");
      if (!ts || !sp || ccord_json_type_of(sp) != CCORD_JSON_OBJECT) {
        pl_reply_err(m, p, id, "invalid_argument", "ui.decorate.set: target and spec required");
        return true;
      }
      ccord_decor_target dt = CCORD_DECOR_MESSAGE_BADGE;
      if (strcmp(ts, "message_badge") == 0) dt = CCORD_DECOR_MESSAGE_BADGE;
      else if (strcmp(ts, "message_accessory") == 0) dt = CCORD_DECOR_MESSAGE_ACCESSORY;
      else if (strcmp(ts, "message_toolbar") == 0) dt = CCORD_DECOR_MESSAGE_TOOLBAR;
      else if (strcmp(ts, "message_style") == 0) dt = CCORD_DECOR_MESSAGE_STYLE;
      else if (strcmp(ts, "member_badge") == 0) dt = CCORD_DECOR_MEMBER_BADGE;
      else if (strcmp(ts, "profile_section") == 0) dt = CCORD_DECOR_PROFILE_SECTION;
      else if (strcmp(ts, "channel_badge") == 0) dt = CCORD_DECOR_CHANNEL_BADGE;
      else if (strcmp(ts, "guild_badge") == 0) dt = CCORD_DECOR_GUILD_BADGE;
      else { pl_reply_err(m, p, id, "invalid_argument", "unknown decorate target"); return true; }

      size_t idl = 0, al = 0, tnl = 0, cll = 0;
      const char *did = pl_str_param(sp, "id", &idl);
      ccord_id tid = pl_parse_id_param(sp, "message_id");
      if (!tid) tid = pl_parse_id_param(sp, "user_id");
      if (!tid) tid = pl_parse_id_param(sp, "channel_id");
      if (!tid) tid = pl_parse_id_param(sp, "guild_id");
      if (!tid) tid = pl_parse_id_param(sp, "target_id");
      const char *acc_s = pl_str_param(sp, "accent_color", &al);
      if (!acc_s) acc_s = pl_str_param(sp, "accent_border", &al);
      const char *tint_s = pl_str_param(sp, "tint_color", &tnl);
      if (!tint_s) tint_s = pl_str_param(sp, "background_tint", &tnl);
      const char *clbl = pl_str_param(sp, "collapse_label", &cll);
      int64_t prio = 0; (void)ccord_json_int64(ccord_json_get(sp, "priority"), &prio);
      ccord_buf wb; ccord_buf_init(&wb, 0);
      const ccord_json *wj = ccord_json_get(sp, "widget");
      if (!wj) wj = ccord_json_get(sp, "root");
      if (wj) ext_encode_json_str(wj, &wb);

      ccord_decorator_contribution dc = {
        .owner = p->m.id, .decor_id = did ? did : "", .target = dt, .target_id = tid,
        .widget_json = wb.len ? ccord_buf_cstr(&wb) : "",
        .accent_rgba = ext_parse_hex_rgba(acc_s), .tint_rgba = ext_parse_hex_rgba(tint_s),
        .collapsed = ccord_json_bool(ccord_json_get(sp, "collapsed"), false),
        .collapse_label = clbl ? clbl : "", .priority = (int32_t)prio,
      };
      ccord_error err = {0};
      ccord_status st = ccord_contrib_set_decorator(m->contrib, &dc, &err);
      ccord_buf_free(&wb);
      if (st != CCORD_OK) pl_reply_err(m, p, id, "invalid_argument", err.message);
      else pl_reply_ok(m, p, id, nullptr);
      return true;
    }
    return true;
  }
  if (strncmp(method, "ui.composer.", 12) == 0) {
    if (!pl_has_perm(p, CCORD_PERM_UI_COMPOSER)) { pl_fail_plugin(m, p, "undeclared restricted API called: ui.composer"); return true; }
    if (strcmp(method, "ui.composer.get_text") == 0 || strcmp(method, "ui.composer.get_selection") == 0 ||
        strcmp(method, "ui.composer.get_reply") == 0 || strcmp(method, "ui.composer.get") == 0) {
      char txt[4096] = "";
      size_t s0 = 0, s1 = 0;
      ccord_id rid = CCORD_ID_NONE;
      bool rping = true;
      (void)ccord_contrib_ui_composer_get(m->contrib, txt, sizeof txt, &s0, &s1, &rid, &rping);
      ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
      if (strcmp(method, "ui.composer.get_text") == 0) {
        ccord_jw_cstr(&w, txt);
      } else if (strcmp(method, "ui.composer.get_selection") == 0) {
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "start"); ccord_jw_int(&w, (int64_t)s0);
        ccord_jw_key(&w, "end"); ccord_jw_int(&w, (int64_t)s1);
        ccord_jw_end_object(&w);
      } else if (strcmp(method, "ui.composer.get_reply") == 0) {
        if (!rid) ccord_jw_null(&w);
        else {
          ccord_jw_begin_object(&w);
          ccord_jw_key(&w, "message_id"); ccord_jw_id(&w, rid);
          ccord_jw_key(&w, "ping"); ccord_jw_bool(&w, rping);
          ccord_jw_end_object(&w);
        }
      } else {
        ccord_jw_begin_object(&w);
        ccord_jw_key(&w, "text"); ccord_jw_cstr(&w, txt);
        ccord_jw_key(&w, "start"); ccord_jw_int(&w, (int64_t)s0);
        ccord_jw_key(&w, "end"); ccord_jw_int(&w, (int64_t)s1);
        if (rid) { ccord_jw_key(&w, "reply_to_id"); ccord_jw_id(&w, rid); ccord_jw_key(&w, "ping"); ccord_jw_bool(&w, rping); }
        ccord_jw_end_object(&w);
      }
      pl_reply_ok(m, p, id, ccord_jw_ok(&w) ? ccord_buf_cstr(&b) : "null");
      ccord_buf_free(&b);
    } else if (strcmp(method, "ui.composer.set_text") == 0) {
      size_t tl = 0; const char *txt = pl_str_param(params, "text", &tl);
      (void)ccord_contrib_ui_composer_set(m->contrib, txt ? txt : "", false, 0);
      pl_reply_ok(m, p, id, nullptr);
    } else if (strcmp(method, "ui.composer.insert") == 0) {
      size_t tl = 0; const char *txt = pl_str_param(params, "text", &tl);
      int64_t pos = -1; (void)ccord_json_int64(ccord_json_get(params, "pos"), &pos);
      (void)ccord_contrib_ui_composer_set(m->contrib, txt ? txt : "", true, (int)pos);
      pl_reply_ok(m, p, id, nullptr);
    } else if (strcmp(method, "ui.composer.set_selection") == 0) {
      int64_t s0 = 0, s1 = 0;
      (void)ccord_json_int64(ccord_json_get(params, "start"), &s0);
      (void)ccord_json_int64(ccord_json_get(params, "end"), &s1);
      (void)ccord_contrib_ui_composer_set_selection(m->contrib, s0 > 0 ? (size_t)s0 : 0, s1 > 0 ? (size_t)s1 : 0);
      pl_reply_ok(m, p, id, nullptr);
    } else if (strcmp(method, "ui.composer.set_reply") == 0) {
      ccord_id mid = pl_parse_id_param(params, "message_id");
      bool ping = ccord_json_bool(ccord_json_get(params, "ping"), true);
      (void)ccord_contrib_ui_composer_set_reply(m->contrib, mid, ping);
      pl_reply_ok(m, p, id, nullptr);
    } else if (strcmp(method, "ui.composer.attach_bytes") == 0) {
      size_t fl = 0, bl = 0;
      const char *fname = pl_str_param(params, "filename", &fl);
      const char *b64 = pl_str_param(params, "b64", &bl);
      bool sp = ccord_json_bool(ccord_json_get(params, "spoiler"), false);
      size_t cap = (bl / 4u) * 3u + 4u;
      uint8_t *raw = bl ? ccord_malloc(cap) : nullptr;
      size_t rlen = 0;
      if (fname && raw && pl_fs_b64_decode((const unsigned char *)b64, bl, raw, &rlen)) {
        (void)ccord_contrib_ui_composer_attach_bytes(m->contrib, fname, raw, rlen, sp);
      }
      ccord_free(raw);
      pl_reply_ok(m, p, id, nullptr);
    } else if (strcmp(method, "ui.composer.register_autocomplete") == 0) {
      size_t il = 0, tl = 0, dl = 0;
      ccord_autocomplete_provider ap = {
        .owner = p->m.id,
        .provider_id = pl_str_param(params, "id", &il),
        .trigger = pl_str_param(params, "trigger", &tl),
        .description = pl_str_param(params, "description", &dl),
      };
      ccord_error err = {0};
      if (ccord_contrib_add_autocomplete(m->contrib, &ap, &err) != CCORD_OK) {
        pl_reply_err(m, p, id, "invalid_argument", err.message);
      } else pl_reply_ok(m, p, id, nullptr);
    } else if (strcmp(method, "ui.composer.unregister_autocomplete") == 0) {
      size_t il = 0; const char *aid = pl_str_param(params, "id", &il);
      if (aid) (void)ccord_contrib_remove_autocomplete(m->contrib, p->m.id, aid);
      pl_reply_ok(m, p, id, nullptr);
    } else {
      pl_reply_err(m, p, id, "unsupported", "unknown composer method");
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- ccord_contrib_handlers callbacks (UI -> plugin)

static size_t parse_ac_items(const char *json, ccord_autocomplete_item *out, size_t max_items)
{
  if (!json || !out || max_items == 0) return 0;
  ccord_arena ar; ccord_arena_init(&ar, 0); ccord_error pe = {0};
  const ccord_json *arr = ccord_json_parse(&ar, json, strlen(json), &k_ext_limits, &pe);
  size_t n = 0;
  if (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) {
    size_t len = ccord_json_array_len(arr);
    for (size_t i = 0; i < len && n < max_items; i++) {
      const ccord_json *it = ccord_json_array_at(arr, i);
      if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
      size_t ll = 0, dl = 0, il = 0, icl = 0;
      const char *lbl = pl_str_param(it, "label", &ll);
      const char *det = pl_str_param(it, "detail", &dl);
      const char *ins = pl_str_param(it, "insert", &il);
      if (!ins) ins = pl_str_param(it, "insert_text", &il);
      const char *ic = pl_str_param(it, "icon", &icl);
      if (!lbl || !lbl[0]) continue;
      memset(&out[n], 0, sizeof out[n]);
      pl_set_text(out[n].label, sizeof out[n].label, lbl);
      pl_set_text(out[n].detail, sizeof out[n].detail, det ? det : "");
      pl_set_text(out[n].insert_text, sizeof out[n].insert_text, ins ? ins : lbl);
      pl_set_text(out[n].icon, sizeof out[n].icon, ic ? ic : "");
      n++;
    }
  }
  ccord_arena_free(&ar);
  return n;
}

size_t plugins_ext_on_command_complete(void *ud, const ccord_command_contribution *cmd, const char *args,
                                       ccord_id guild_id, ccord_id channel_id, ccord_autocomplete_item *out,
                                       size_t max_items)
{
  ccord_plugins *m = ud;
  plugin *p = cmd ? pl_find_owner(m, cmd->owner) : nullptr;
  if (!p || p->state != CCORD_PLUGIN_READY) return 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "name"); ccord_jw_cstr(&w, cmd->name);
  ccord_jw_key(&w, "args"); ccord_jw_cstr(&w, args ? args : "");
  ccord_jw_key(&w, "ctx");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, guild_id);
  ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, channel_id);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  char *res = nullptr; char eb[128];
  size_t got = 0;
  if (ccord_jw_ok(&w) && ext_sync_host_call(m, p, "command.complete", ccord_buf_cstr(&b), 120u, &res, eb, sizeof eb)) {
    got = parse_ac_items(res, out, max_items);
  }
  ccord_free(res);
  ccord_buf_free(&b);
  return got;
}

size_t plugins_ext_on_autocomplete_query(void *ud, const char *owner, const char *provider_id, const char *query,
                                         ccord_id guild_id, ccord_id channel_id, ccord_autocomplete_item *out,
                                         size_t max_items)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, owner);
  if (!p || p->state != CCORD_PLUGIN_READY) return 0;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "provider_id"); ccord_jw_cstr(&w, provider_id ? provider_id : "");
  ccord_jw_key(&w, "query"); ccord_jw_cstr(&w, query ? query : "");
  ccord_jw_key(&w, "ctx");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, guild_id);
  ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, channel_id);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  char *res = nullptr; char eb[128];
  size_t got = 0;
  if (ccord_jw_ok(&w) && ext_sync_host_call(m, p, "composer.autocomplete", ccord_buf_cstr(&b), 120u, &res, eb, sizeof eb)) {
    got = parse_ac_items(res, out, max_items);
  }
  ccord_free(res);
  ccord_buf_free(&b);
  return got;
}

void plugins_ext_on_decor_event(void *ud, const ccord_decorator_contribution *decor, ccord_id target_id,
                                const ccord_panel_event *ev)
{
  ccord_plugins *m = ud;
  plugin *p = decor ? pl_find_owner(m, decor->owner) : nullptr;
  if (!p || p->state != CCORD_PLUGIN_READY || !ev) return;
  char tid[24] = "";
  if (target_id) ccord_id_format(target_id, tid);
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "decor_id"); ccord_jw_cstr(&w, decor->decor_id);
  ccord_jw_key(&w, "target_id"); ccord_jw_cstr(&w, tid);
  ccord_jw_key(&w, "event");
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "id"); ccord_jw_cstr(&w, ev->node_id ? ev->node_id : "");
  ccord_jw_key(&w, "type"); ccord_jw_cstr(&w, ev->kind == CCORD_PANEL_EV_RIGHT_CLICK ? "right_click" : "click");
  ccord_jw_key(&w, "x"); ccord_jw_double(&w, (double)ev->x);
  ccord_jw_key(&w, "y"); ccord_jw_double(&w, (double)ev->y);
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "decor.event", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

void plugins_ext_on_setting_changed(void *ud, const char *owner, const char *key, const char *value_json)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, owner);
  if (!p || !key || !value_json) return;
  if (pl_valid_store_key(key, strlen(key))) {
    (void)pl_store_put(p, key, value_json, strlen(value_json));
  }
  if (p->state != CCORD_PLUGIN_READY) return;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_arena ar; ccord_arena_init(&ar, 0); ccord_error pe = {0};
  const ccord_json *vj = ccord_json_parse(&ar, value_json, strlen(value_json), &k_ext_limits, &pe);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "key"); ccord_jw_cstr(&w, key);
  ccord_jw_key(&w, "value");
  if (vj) pl_jw_copy_json(&w, vj); else ccord_jw_null(&w);
  ccord_jw_end_object(&w);
  ccord_arena_free(&ar);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "settings.change", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

void plugins_ext_on_keybind_triggered(void *ud, const char *owner, const char *bind_id)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, owner);
  if (!p || p->state != CCORD_PLUGIN_READY || !bind_id) return;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "bind_id"); ccord_jw_cstr(&w, bind_id);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "keybind.invoke", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

void plugins_ext_on_action_btn_click(void *ud, const ccord_action_btn_contribution *btn, const ccord_menu_context *ctx)
{
  ccord_plugins *m = ud;
  plugin *p = btn ? pl_find_owner(m, btn->owner) : nullptr;
  if (!p || p->state != CCORD_PLUGIN_READY) return;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "btn_id"); ccord_jw_cstr(&w, btn->btn_id);
  ccord_jw_key(&w, "ctx");
  ccord_jw_begin_object(&w);
  if (ctx) {
    if (ctx->guild_id) { ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, ctx->guild_id); }
    if (ctx->channel_id) { ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, ctx->channel_id); }
    if (ctx->message_id) { ccord_jw_key(&w, "message_id"); ccord_jw_id(&w, ctx->message_id); }
    if (ctx->user_id) { ccord_jw_key(&w, "user_id"); ccord_jw_id(&w, ctx->user_id); }
  }
  ccord_jw_end_object(&w);
  ccord_jw_end_object(&w);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "action_btn.click", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

void plugins_ext_on_dialog_result(void *ud, const char *owner, uint64_t req_id, bool accepted, const char *result_json)
{
  ccord_plugins *m = ud;
  plugin *p = pl_find_owner(m, owner);
  if (!p || p->state != CCORD_PLUGIN_READY) return;
  ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
  ccord_arena ar; ccord_arena_init(&ar, 0);
  const ccord_json *rj = nullptr;
  if (result_json && result_json[0]) {
    ccord_error pe = {0};
    rj = ccord_json_parse(&ar, result_json, strlen(result_json), &k_ext_limits, &pe);
  }
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "req_id"); ccord_jw_int(&w, (int64_t)req_id);
  ccord_jw_key(&w, "accepted"); ccord_jw_bool(&w, accepted);
  ccord_jw_key(&w, "result");
  if (rj) pl_jw_copy_json(&w, rj); else ccord_jw_null(&w);
  ccord_jw_end_object(&w);
  ccord_arena_free(&ar);
  if (ccord_jw_ok(&w)) pl_send_ntf(m, p, "dialog.result", ccord_buf_cstr(&b));
  ccord_buf_free(&b);
}

bool plugins_ext_on_before_send(void *ud, ccord_id channel_id, ccord_id reply_to_id, const char *text,
                                char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap)
{
  ccord_plugins *m = ud;
  pl_set_text(out_text, out_cap, text);
  if (cancel_reason && reason_cap) cancel_reason[0] = '\0';
  if (!m) return true;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    if (!p->used || p->state != CCORD_PLUGIN_READY || !(p->hook_mask & HOOK_BEFORE_SEND)) continue;
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "hook"); ccord_jw_cstr(&w, "before_send");
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, channel_id);
    if (reply_to_id) { ccord_jw_key(&w, "reply_to_id"); ccord_jw_id(&w, reply_to_id); }
    ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, out_text);
    ccord_jw_end_object(&w);
    ccord_jw_end_object(&w);
    char *res = nullptr; char eb[128];
    bool ok = ccord_jw_ok(&w) && ext_sync_host_call(m, p, "hook.call", ccord_buf_cstr(&b), 150u, &res, eb, sizeof eb);
    ccord_buf_free(&b);
    if (ok && res) {
      ccord_arena ar; ccord_arena_init(&ar, 0); ccord_error pe = {0};
      const ccord_json *rj = ccord_json_parse(&ar, res, strlen(res), &k_ext_limits, &pe);
      if (rj) {
        if (ccord_json_type_of(rj) == CCORD_JSON_BOOL && !ccord_json_bool(rj, true)) {
          ccord_arena_free(&ar); ccord_free(res);
          return false;
        }
        if (ccord_json_type_of(rj) == CCORD_JSON_STRING) {
          size_t sl = 0; const char *ns = ccord_json_string(rj, &sl);
          if (ns) pl_set_text(out_text, out_cap, ns);
        } else if (ccord_json_type_of(rj) == CCORD_JSON_OBJECT) {
          if (ccord_json_bool(ccord_json_get(rj, "cancel"), false)) {
            size_t rl = 0; const char *rsn = pl_str_param(rj, "reason", &rl);
            if (rsn && cancel_reason && reason_cap) pl_set_text(cancel_reason, reason_cap, rsn);
            ccord_arena_free(&ar); ccord_free(res);
            return false;
          }
          size_t cl = 0; const char *nc = pl_str_param(rj, "content", &cl);
          if (nc) pl_set_text(out_text, out_cap, nc);
        }
      }
      ccord_arena_free(&ar);
    }
    ccord_free(res);
  }
  return true;
}

bool plugins_ext_on_before_edit(void *ud, ccord_id channel_id, ccord_id message_id, const char *text,
                                char *out_text, size_t out_cap, char *cancel_reason, size_t reason_cap)
{
  ccord_plugins *m = ud;
  pl_set_text(out_text, out_cap, text);
  if (cancel_reason && reason_cap) cancel_reason[0] = '\0';
  if (!m) return true;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    if (!p->used || p->state != CCORD_PLUGIN_READY || !(p->hook_mask & HOOK_BEFORE_EDIT)) continue;
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "hook"); ccord_jw_cstr(&w, "before_edit");
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, channel_id);
    ccord_jw_key(&w, "message_id"); ccord_jw_id(&w, message_id);
    ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, out_text);
    ccord_jw_end_object(&w);
    ccord_jw_end_object(&w);
    char *res = nullptr; char eb[128];
    bool ok = ccord_jw_ok(&w) && ext_sync_host_call(m, p, "hook.call", ccord_buf_cstr(&b), 150u, &res, eb, sizeof eb);
    ccord_buf_free(&b);
    if (ok && res) {
      ccord_arena ar; ccord_arena_init(&ar, 0); ccord_error pe = {0};
      const ccord_json *rj = ccord_json_parse(&ar, res, strlen(res), &k_ext_limits, &pe);
      if (rj) {
        if (ccord_json_type_of(rj) == CCORD_JSON_BOOL && !ccord_json_bool(rj, true)) {
          ccord_arena_free(&ar); ccord_free(res); return false;
        }
        if (ccord_json_type_of(rj) == CCORD_JSON_STRING) {
          size_t sl = 0; const char *ns = ccord_json_string(rj, &sl);
          if (ns) pl_set_text(out_text, out_cap, ns);
        } else if (ccord_json_type_of(rj) == CCORD_JSON_OBJECT) {
          if (ccord_json_bool(ccord_json_get(rj, "cancel"), false)) {
            size_t rl = 0; const char *rsn = pl_str_param(rj, "reason", &rl);
            if (rsn && cancel_reason && reason_cap) pl_set_text(cancel_reason, reason_cap, rsn);
            ccord_arena_free(&ar); ccord_free(res); return false;
          }
          size_t cl = 0; const char *nc = pl_str_param(rj, "content", &cl);
          if (nc) pl_set_text(out_text, out_cap, nc);
        }
      }
      ccord_arena_free(&ar);
    }
    ccord_free(res);
  }
  return true;
}

bool plugins_ext_on_notification_hook(void *ud, ccord_id guild_id, ccord_id channel_id, ccord_id message_id,
                                      ccord_id author_id, const char *content, bool is_mention, bool is_dm,
                                      char *out_title, size_t title_cap, char *out_body, size_t body_cap,
                                      bool *out_silent)
{
  ccord_plugins *m = ud;
  if (!m) return true;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    if (!p->used || p->state != CCORD_PLUGIN_READY || !(p->hook_mask & HOOK_NOTIFICATION)) continue;
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "hook"); ccord_jw_cstr(&w, "notification");
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "guild_id"); ccord_jw_id(&w, guild_id);
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, channel_id);
    ccord_jw_key(&w, "message_id"); ccord_jw_id(&w, message_id);
    ccord_jw_key(&w, "author_id"); ccord_jw_id(&w, author_id);
    ccord_jw_key(&w, "content"); ccord_jw_cstr(&w, content ? content : "");
    ccord_jw_key(&w, "title"); ccord_jw_cstr(&w, out_title ? out_title : "");
    ccord_jw_key(&w, "body"); ccord_jw_cstr(&w, out_body ? out_body : "");
    ccord_jw_key(&w, "is_mention"); ccord_jw_bool(&w, is_mention);
    ccord_jw_key(&w, "is_dm"); ccord_jw_bool(&w, is_dm);
    ccord_jw_end_object(&w);
    ccord_jw_end_object(&w);
    char *res = nullptr; char eb[128];
    bool ok = ccord_jw_ok(&w) && ext_sync_host_call(m, p, "hook.call", ccord_buf_cstr(&b), 120u, &res, eb, sizeof eb);
    ccord_buf_free(&b);
    if (ok && res) {
      ccord_arena ar; ccord_arena_init(&ar, 0); ccord_error pe = {0};
      const ccord_json *rj = ccord_json_parse(&ar, res, strlen(res), &k_ext_limits, &pe);
      if (rj) {
        if (ccord_json_type_of(rj) == CCORD_JSON_BOOL && !ccord_json_bool(rj, true)) {
          ccord_arena_free(&ar); ccord_free(res); return false;
        }
        if (ccord_json_type_of(rj) == CCORD_JSON_OBJECT) {
          if (ccord_json_bool(ccord_json_get(rj, "suppress"), false) ||
              ccord_json_bool(ccord_json_get(rj, "cancel"), false)) {
            ccord_arena_free(&ar); ccord_free(res); return false;
          }
          size_t tl = 0, bl = 0;
          const char *nt = pl_str_param(rj, "title", &tl);
          const char *nb = pl_str_param(rj, "body", &bl);
          if (nt && out_title && title_cap) pl_set_text(out_title, title_cap, nt);
          if (nb && out_body && body_cap) pl_set_text(out_body, body_cap, nb);
          if (out_silent && ccord_json_get(rj, "silent")) {
            *out_silent = ccord_json_bool(ccord_json_get(rj, "silent"), *out_silent);
          }
        }
      }
      ccord_arena_free(&ar);
    }
    ccord_free(res);
  }
  return true;
}

static bool link_extract_http_host(const char *url, char *host_out, size_t host_cap)
{
  if (!url || !host_out || host_cap == 0) return false;
  for (const unsigned char *q = (const unsigned char *)url; *q; q++) {
    if (*q < 0x20u || *q == 0x7fu) return false;
  }
  const char *auth = nullptr;
  if (strncmp(url, "https://", 8) == 0) auth = url + 8;
  else if (strncmp(url, "http://", 7) == 0) auth = url + 7;
  else return false;
  const char *p = auth;
  while (*p && *p != '/' && *p != '?' && *p != '#') {
    unsigned char c = (unsigned char)*p;
    if (c == '@' || c == '\\' || c <= 0x20u || c >= 0x7fu) return false;
    p++;
  }
  const char *hs = auth;
  const char *he = p;
  bool is_v6 = false;
  const char *port_s = nullptr;
  if (hs < p && *hs == '[') {
    const char *rb = memchr(hs, ']', (size_t)(p - hs));
    if (!rb) return false;
    is_v6 = true;
    hs++;
    he = rb;
    if (rb + 1 < p) {
      if (rb[1] != ':') return false;
      port_s = rb + 2;
    }
  } else {
    const char *col = memchr(hs, ':', (size_t)(p - hs));
    if (col) {
      if (memchr(col + 1, ':', (size_t)(p - (col + 1))) != nullptr) return false;
      he = col;
      port_s = col + 1;
    }
  }
  if (port_s) {
    if (port_s >= p) return false;
    unsigned long pv = 0;
    for (const char *q = port_s; q < p; q++) {
      if (*q < '0' || *q > '9') return false;
      pv = pv * 10ul + (unsigned long)(*q - '0');
      if (pv > 65535ul) return false;
    }
    if (pv == 0ul) return false;
  }
  size_t hl = (size_t)(he - hs);
  if (hl == 0 || hl >= host_cap) return false;
  for (size_t i = 0; i < hl; i++) {
    unsigned char c = (unsigned char)hs[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || (is_v6 && c == ':');
    if (!ok) return false;
    host_out[i] = (char)tolower(c);
  }
  host_out[hl] = '\0';
  return true;
}

static bool link_rewrite_allowed(const char *orig_url, const char *new_url)
{
  char oh[256], nh[256];
  if (!link_extract_http_host(orig_url, oh, sizeof oh)) return false;
  if (!link_extract_http_host(new_url, nh, sizeof nh)) return false;
  return strcmp(oh, nh) == 0;
}

bool plugins_ext_on_link_open_hook(void *ud, const char *url, char *out_url, size_t out_cap)
{
  ccord_plugins *m = ud;
  pl_set_text(out_url, out_cap, url);
  if (!m) return true;
  for (size_t i = 0; i < m->nplugins; i++) {
    plugin *p = m->plugins[i];
    if (!p->used || p->state != CCORD_PLUGIN_READY || !(p->hook_mask & HOOK_LINK_OPEN)) continue;
    ccord_buf b; ccord_buf_init(&b, 0); ccord_jw w; ccord_jw_init(&w, &b);
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "hook"); ccord_jw_cstr(&w, "link_open");
    ccord_jw_key(&w, "data");
    ccord_jw_begin_object(&w);
    ccord_jw_key(&w, "url"); ccord_jw_cstr(&w, out_url);
    ccord_jw_end_object(&w);
    ccord_jw_end_object(&w);
    char *res = nullptr; char eb[128];
    bool ok = ccord_jw_ok(&w) && ext_sync_host_call(m, p, "hook.call", ccord_buf_cstr(&b), 120u, &res, eb, sizeof eb);
    ccord_buf_free(&b);
    if (ok && res) {
      ccord_arena ar; ccord_arena_init(&ar, 0); ccord_error pe = {0};
      const ccord_json *rj = ccord_json_parse(&ar, res, strlen(res), &k_ext_limits, &pe);
      if (rj) {
        if (ccord_json_type_of(rj) == CCORD_JSON_BOOL && !ccord_json_bool(rj, true)) {
          ccord_arena_free(&ar); ccord_free(res); return false;
        }
        if (ccord_json_type_of(rj) == CCORD_JSON_STRING) {
          size_t sl = 0; const char *nu = ccord_json_string(rj, &sl);
          if (nu && link_rewrite_allowed(url, nu)) {
            pl_set_text(out_url, out_cap, nu);
          }
        } else if (ccord_json_type_of(rj) == CCORD_JSON_OBJECT) {
          if (ccord_json_bool(ccord_json_get(rj, "cancel"), false)) {
            ccord_arena_free(&ar); ccord_free(res); return false;
          }
          size_t ul = 0; const char *nu = pl_str_param(rj, "url", &ul);
          if (nu && link_rewrite_allowed(url, nu)) {
            pl_set_text(out_url, out_cap, nu);
          }
        }
      }
      ccord_arena_free(&ar);
    }
    ccord_free(res);
  }
  return true;
}

