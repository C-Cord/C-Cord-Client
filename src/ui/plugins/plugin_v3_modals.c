// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "plugin_v3_internal.h"

// ---- Shared Form Field Parser & Text Input Helpers ----

static void v3_init_edit_str(v3_field *f, const char *s)
{
  if (!f->edit_inited) {
    ccord_text_edit_init(&f->edit, 1024);
    f->edit_inited = true;
  } else {
    ccord_text_edit_clear(&f->edit);
  }
  if (s && s[0]) {
    ccord_error err = {0};
    (void)ccord_text_edit_insert(&f->edit, s, strlen(s), &err);
  }
}

static bool v3_handle_text_edit_event(ccord_ui *ui, ccord_text_edit *ed, bool masked, const ccord_input_event *ev)
{
  if (!ed || !ev) return false;
  if (ev->type == CCORD_IN_TEXT && ev->text[0]) {
    ccord_error err = {0};
    (void)ccord_text_edit_insert(ed, ev->text, strlen(ev->text), &err);
    return true;
  }
  if (ev->type != CCORD_IN_KEY_DOWN) return false;
  bool ctrl = (ev->mods & (CCORD_MOD_CTRL | CCORD_MOD_SUPER)) != 0;
  bool shift = (ev->mods & CCORD_MOD_SHIFT) != 0;
  switch (ev->key) {
  case CCORD_KEY_BACKSPACE:
    if (ctrl) ccord_text_edit_backspace_word(ed);
    else ccord_text_edit_backspace(ed);
    return true;
  case CCORD_KEY_DELETE:
    if (ctrl) ccord_text_edit_delete_word(ed);
    else ccord_text_edit_delete(ed);
    return true;
  case CCORD_KEY_LEFT:
    ccord_text_edit_move(ed, ctrl ? -3 : -1, shift);
    return true;
  case CCORD_KEY_RIGHT:
    ccord_text_edit_move(ed, ctrl ? 3 : 1, shift);
    return true;
  case CCORD_KEY_HOME:
    ccord_text_edit_move(ed, -2, shift);
    return true;
  case CCORD_KEY_END:
    ccord_text_edit_move(ed, 2, shift);
    return true;
  case CCORD_KEY_A:
    if (ctrl) { ccord_text_edit_select_all(ed); return true; }
    break;
  case CCORD_KEY_C:
    if (ctrl && !masked && ui->d.host.clipboard_set) {
      char *sel = ccord_text_edit_copy_selection(ed);
      if (sel) { ui->d.host.clipboard_set(ui->d.host.ud, sel); ccord_free(sel); }
      return true;
    }
    break;
  case CCORD_KEY_X:
    if (ctrl && !masked && ui->d.host.clipboard_set) {
      char *sel = ccord_text_edit_copy_selection(ed);
      if (sel) {
        ui->d.host.clipboard_set(ui->d.host.ud, sel);
        ccord_free(sel);
        ccord_text_edit_backspace(ed);
      }
      return true;
    }
    break;
  case CCORD_KEY_V:
    if (ctrl && ui->d.host.clipboard_get) {
      char *clip = ui->d.host.clipboard_get(ui->d.host.ud);
      if (clip) {
        ccord_error err = {0};
        (void)ccord_text_edit_insert(ed, clip, strlen(clip), &err);
        ccord_free(clip);
      }
      return true;
    }
    break;
  case CCORD_KEY_Z:
    if (ctrl) {
      if (shift) (void)ccord_text_edit_redo(ed);
      else (void)ccord_text_edit_undo(ed);
      return true;
    }
    break;
  default:
    break;
  }
  return false;
}

static void v3_parse_field_list(const char *schema_json, const char *values_json,
                                v3_field *fields, size_t *nfields)
{
  pv3_v3_clear_fields(fields, nfields);
  if (!schema_json || !schema_json[0]) return;

  ccord_arena arena = {0};
  ccord_arena_init(&arena, 8192);
  ccord_error err = {0};
  const ccord_json *root = ccord_json_parse(&arena, schema_json, strlen(schema_json), nullptr, &err);
  const ccord_json *arr = nullptr;
  if (root) {
    if (ccord_json_type_of(root) == CCORD_JSON_ARRAY) arr = root;
    else if (ccord_json_type_of(root) == CCORD_JSON_OBJECT) arr = ccord_json_get(root, "fields");
  }
  const ccord_json *vals = nullptr;
  if (values_json && values_json[0]) {
    vals = ccord_json_parse(&arena, values_json, strlen(values_json), nullptr, &err);
    if (vals && ccord_json_type_of(vals) != CCORD_JSON_OBJECT) vals = nullptr;
  }

  if (arr && ccord_json_type_of(arr) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(arr);
    for (size_t i = 0; i < n && *nfields < V3_MAX_FORM_FIELDS; i++) {
      const ccord_json *it = ccord_json_array_at(arr, i);
      if (!it || ccord_json_type_of(it) != CCORD_JSON_OBJECT) continue;
      v3_field *f = &fields[*nfields];
      memset(f, 0, sizeof *f);

      const char *type = ccord_json_string(ccord_json_get(it, "type"), nullptr);
      const char *key = ccord_json_string(ccord_json_get(it, "key"), nullptr);
      const char *label = ccord_json_string(ccord_json_get(it, "label"), nullptr);
      const char *desc = ccord_json_string(ccord_json_get(it, "description"), nullptr);
      const char *ph = ccord_json_string(ccord_json_get(it, "placeholder"), nullptr);

      if (!type) type = "text";
      if (strcmp(type, "section") == 0) f->kind = V3F_SECTION;
      else if (strcmp(type, "toggle") == 0 || strcmp(type, "bool") == 0 || strcmp(type, "boolean") == 0) f->kind = V3F_TOGGLE;
      else if (strcmp(type, "slider") == 0 || strcmp(type, "number") == 0) f->kind = V3F_SLIDER;
      else if (strcmp(type, "select") == 0) f->kind = V3F_SELECT;
      else if (strcmp(type, "color") == 0) f->kind = V3F_COLOR;
      else if (strcmp(type, "secret") == 0 || strcmp(type, "password") == 0) f->kind = V3F_SECRET;
      else f->kind = V3F_TEXT;

      snprintf(f->key, sizeof f->key, "%s", key ? key : "");
      snprintf(f->label, sizeof f->label, "%s", label ? label : (key ? key : ""));
      snprintf(f->desc, sizeof f->desc, "%s", desc ? desc : "");
      snprintf(f->placeholder, sizeof f->placeholder, "%s", ph ? ph : "");

      const ccord_json *def = ccord_json_get(it, "default");
      const ccord_json *cur = (vals && f->key[0]) ? ccord_json_get(vals, f->key) : nullptr;
      const ccord_json *val = cur ? cur : def;

      if (f->kind == V3F_TOGGLE) {
        f->bool_val = ccord_json_bool(val, false);
      } else if (f->kind == V3F_SLIDER) {
        f->num_min = ccord_json_number(ccord_json_get(it, "min"), 0.0);
        f->num_max = ccord_json_number(ccord_json_get(it, "max"), 100.0);
        if (f->num_max <= f->num_min) f->num_max = f->num_min + 100.0;
        f->num_step = ccord_json_number(ccord_json_get(it, "step"), 1.0);
        f->num_val = ccord_json_number(val, f->num_min);
        if (f->num_val < f->num_min) f->num_val = f->num_min;
        if (f->num_val > f->num_max) f->num_val = f->num_max;
      } else if (f->kind == V3F_SELECT) {
        const ccord_json *opts = ccord_json_get(it, "options");
        if (opts && ccord_json_type_of(opts) == CCORD_JSON_ARRAY) {
          size_t no = ccord_json_array_len(opts);
          for (size_t oi = 0; oi < no && f->nopts < V3_MAX_FIELD_OPTS; oi++) {
            const ccord_json *o = ccord_json_array_at(opts, oi);
            if (ccord_json_type_of(o) == CCORD_JSON_STRING) {
              const char *os = ccord_json_string(o, nullptr);
              snprintf(f->opts[f->nopts].label, sizeof f->opts[0].label, "%s", os ? os : "");
              snprintf(f->opts[f->nopts].value, sizeof f->opts[0].value, "%s", os ? os : "");
              f->nopts++;
            } else if (ccord_json_type_of(o) == CCORD_JSON_OBJECT) {
              const char *ol = ccord_json_string(ccord_json_get(o, "label"), nullptr);
              const char *ov = ccord_json_string(ccord_json_get(o, "value"), nullptr);
              snprintf(f->opts[f->nopts].label, sizeof f->opts[0].label, "%s", ol ? ol : (ov ? ov : ""));
              snprintf(f->opts[f->nopts].value, sizeof f->opts[0].value, "%s", ov ? ov : (ol ? ol : ""));
              f->nopts++;
            }
          }
        }
        const char *vs = ccord_json_string(val, nullptr);
        f->sel_idx = 0;
        if (vs) {
          for (size_t oi = 0; oi < f->nopts; oi++) {
            if (strcmp(f->opts[oi].value, vs) == 0 || strcmp(f->opts[oi].label, vs) == 0) {
              f->sel_idx = oi;
              break;
            }
          }
        }
      } else if (f->kind == V3F_TEXT || f->kind == V3F_SECRET || f->kind == V3F_COLOR) {
        const char *vs = ccord_json_string(val, nullptr);
        if (!vs && f->kind == V3F_COLOR) vs = "#5865f2";
        v3_init_edit_str(f, vs ? vs : "");
      }
      (*nfields)++;
    }
  }
  ccord_arena_free(&arena);
}

static float v3_field_height(const v3_field *f)
{
  if (f->kind == V3F_SECTION) return 28.0f;
  if (f->kind == V3F_TOGGLE) return f->desc[0] ? 48.0f : 36.0f;
  if (f->kind == V3F_SLIDER) return 54.0f;
  return f->desc[0] ? 74.0f : 60.0f;
}

static void v3_draw_field(ccord_ui *ui, ccord_renderer *r, v3_field *f, size_t idx,
                          float x, float y, float w, bool focused, const char *ns)
{
  uis_tokens tk = uis_tokens_of(ui);
  uint64_t aid = ui_name_id(f->key[0] ? f->key : f->label) ^ (uint64_t)(idx + 1u);

  if (f->kind == V3F_SECTION) {
    ccord_text_style sst = ui_style(ui, CCORD_FONT_BOLD, tk.fs_overline, ui->theme->c.text_muted);
    ccord_text_draw(r, f->label, strlen(f->label), x, y + 6.0f, &sst, 0.0f);
    uis_divider(ui, r, x, y + 24.0f, w);
    f->hit_rect = (ccord_rect){0, 0, 0, 0};
    return;
  }

  ccord_text_style lst = ui_style(ui, CCORD_FONT_BOLD, tk.fs_control, ui->theme->c.text_header);
  ccord_text_style dst = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_caption, ui->theme->c.text_muted);

  if (f->kind == V3F_TOGGLE) {
    ccord_rect row = {x, y, w, 32.0f};
    ccord_text_draw(r, f->label, strlen(f->label), x, y + 6.0f, &lst, 0.0f);
    ccord_rect trc = uis_toggle_rect(row);
    bool hov = ui_in_rect(ui->mx, ui->my, trc);
    uis_toggle(ui, r, row, f->bool_val, (uis_state){.hover = hov}, ns, aid);
    if (f->desc[0]) ccord_text_draw(r, f->desc, strlen(f->desc), x, y + 26.0f, &dst, w - UIS_TOGGLE_W - UIS_SP_2);
    f->hit_rect = trc;
    return;
  }

  ccord_text_draw(r, f->label, strlen(f->label), x, y, &lst, 0.0f);
  float cy = y + 20.0f;

  if (f->kind == V3F_SLIDER) {
    char vbuf[32];
    if (fabs(f->num_val - round(f->num_val)) < 1e-6) snprintf(vbuf, sizeof vbuf, "%.0f", f->num_val);
    else snprintf(vbuf, sizeof vbuf, "%.2f", f->num_val);
    ccord_vec2 vm = ccord_text_measure(r, vbuf, strlen(vbuf), &dst, 0.0f);
    ccord_text_draw(r, vbuf, strlen(vbuf), x + w - vm.x, y, &dst, 0.0f);

    ccord_rect track = {x, cy + 8.0f, w, UIS_SLIDER_H};
    float frac = (float)((f->num_val - f->num_min) / (f->num_max - f->num_min));
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    uis_slider(ui, r, track, frac, (uis_state){.hover = ui_in_rect(ui->mx, ui->my, track)});
    f->hit_rect = (ccord_rect){x, cy, w, 24.0f};
    return;
  }

  if (f->kind == V3F_SELECT) {
    ccord_rect box = {x, cy, w, UIS_H_INPUT};
    const char *cur = (f->nopts > 0 && f->sel_idx < f->nopts) ? f->opts[f->sel_idx].label : "";
    bool hov = ui_in_rect(ui->mx, ui->my, box);
    uis_select(ui, r, box, cur, (uis_state){.hover = hov, .focused = focused}, ns, aid);
    f->hit_rect = box;
    if (f->desc[0]) ccord_text_draw(r, f->desc, strlen(f->desc), x, cy + UIS_H_INPUT + 2.0f, &dst, w);
    return;
  }

  if (f->kind == V3F_COLOR) {
    ccord_color swatch = ui->theme->c.brand;
    (void)pv3_v3_parse_hex_color(ccord_text_edit_cstr(&f->edit), &swatch);
    ccord_rect sw = {x, cy + (UIS_H_INPUT - UIS_SWATCH_D) * 0.5f, UIS_SWATCH_D, UIS_SWATCH_D};
    uis_swatch(ui, r, sw, swatch, (uis_state){.selected = focused}, ns, aid);
    ccord_rect box = {x + UIS_SWATCH_D + UIS_SP_2, cy, w - UIS_SWATCH_D - UIS_SP_2, UIS_H_INPUT};
    bool hov = ui_in_rect(ui->mx, ui->my, box);
    (void)uis_input(ui, r, box, &f->edit, "#RRGGBB", (uis_state){.hover = hov, .focused = focused}, false, ns, aid ^ 1u);
    f->hit_rect = box;
    if (f->desc[0]) ccord_text_draw(r, f->desc, strlen(f->desc), x, cy + UIS_H_INPUT + 2.0f, &dst, w);
    return;
  }

  // V3F_TEXT or V3F_SECRET
  ccord_rect box = {x, cy, w, UIS_H_INPUT};
  bool hov = ui_in_rect(ui->mx, ui->my, box);
  (void)uis_input(ui, r, box, &f->edit, f->placeholder,
                  (uis_state){.hover = hov, .focused = focused}, f->kind == V3F_SECRET, ns, aid);
  f->hit_rect = box;
  if (f->desc[0]) ccord_text_draw(r, f->desc, strlen(f->desc), x, cy + UIS_H_INPUT + 2.0f, &dst, w);
}

static double v3_slider_from_x(const v3_field *f, float x)
{
  float frac = f->hit_rect.w > 0.0f ? (x - f->hit_rect.x) / f->hit_rect.w : 0.0f;
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  double v = f->num_min + (double)frac * (f->num_max - f->num_min);
  if (f->num_step > 0.0) v = f->num_min + round((v - f->num_min) / f->num_step) * f->num_step;
  if (v < f->num_min) v = f->num_min;
  if (v > f->num_max) v = f->num_max;
  return v;
}

// ---- User-Initiated Plugin Dialog Modal (confirm / prompt / form) ----

static void v3_dialog_sync(ccord_ui *ui, const ccord_plugin_dialog *dlg)
{
  ui_plugin_v3 *v = pv3_v3_ensure(ui);
  if (!v || !dlg) return;
  if (v->dlg.active_req_id == dlg->req_id) return;

  pv3_v3_clear_fields(v->dlg.fields, &v->dlg.nfields);
  v->dlg.active_req_id = dlg->req_id;
  v->dlg.kind = dlg->kind;
  v->dlg.focused_idx = -1;
  v->dlg.drag_slider_idx = -1;
  v->dlg.scroll_y = 0.0f;
  v->dlg.content_h = 0.0f;
  v->dlg.body_rect = (ccord_rect){0};

  if (dlg->kind == CCORD_DIALOG_PROMPT) {
    v3_field *f = &v->dlg.fields[0];
    memset(f, 0, sizeof *f);
    f->kind = V3F_TEXT;
    snprintf(f->key, sizeof f->key, "value");
    snprintf(f->label, sizeof f->label, "Value");
    v3_init_edit_str(f, dlg->default_value ? dlg->default_value : "");
    v->dlg.nfields = 1;
    v->dlg.focused_idx = 0;
  } else if (dlg->kind == CCORD_DIALOG_FORM) {
    v3_parse_field_list(dlg->fields_json, nullptr, v->dlg.fields, &v->dlg.nfields);
    for (size_t i = 0; i < v->dlg.nfields; i++) {
      if (v->dlg.fields[i].kind == V3F_TEXT || v->dlg.fields[i].kind == V3F_SECRET || v->dlg.fields[i].kind == V3F_COLOR) {
        v->dlg.focused_idx = (int)i;
        break;
      }
    }
  }
}

static void v3_dialog_submit(ccord_ui *ui, const ccord_plugin_dialog *dlg, bool accepted)
{
  ui_plugin_v3 *v = ui->plugin_v3;
  if (!ui->d.contrib || !dlg || !v) return;
  if (!accepted) {
    ccord_contrib_resolve_dialog(ui->d.contrib, dlg->req_id, false, nullptr);
    v->dlg.active_req_id = 0;
    if (ui->touch_mode) ui->touch_text_focused = false;
    ui->dirty = true;
    return;
  }

  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  if (dlg->kind == CCORD_DIALOG_CONFIRM) {
    ccord_jw_bool(&w, true);
  } else if (dlg->kind == CCORD_DIALOG_PROMPT) {
    const char *txt = (v->dlg.nfields > 0) ? ccord_text_edit_cstr(&v->dlg.fields[0].edit) : "";
    ccord_jw_cstr(&w, txt);
  } else {
    ccord_jw_begin_object(&w);
    for (size_t i = 0; i < v->dlg.nfields; i++) {
      const v3_field *f = &v->dlg.fields[i];
      if (f->kind == V3F_SECTION || !f->key[0]) continue;
      ccord_jw_key(&w, f->key);
      if (f->kind == V3F_TOGGLE) ccord_jw_bool(&w, f->bool_val);
      else if (f->kind == V3F_SLIDER) ccord_jw_double(&w, f->num_val);
      else if (f->kind == V3F_SELECT) {
        const char *sv = (f->nopts > 0 && f->sel_idx < f->nopts) ? f->opts[f->sel_idx].value : "";
        ccord_jw_cstr(&w, sv);
      } else {
        ccord_jw_cstr(&w, ccord_text_edit_cstr(&f->edit));
      }
    }
    ccord_jw_end_object(&w);
  }
  ccord_contrib_resolve_dialog(ui->d.contrib, dlg->req_id, true, (b.data && ccord_jw_ok(&w)) ? (const char *)b.data : "true");
  ccord_buf_free(&b);
  v->dlg.active_req_id = 0;
  if (ui->touch_mode) ui->touch_text_focused = false;
  ui->dirty = true;
}

bool ui_plugin_dialog_is_open(const ccord_ui *ui)
{
  if (!ui || !ui->d.contrib) return false;
  return ccord_contrib_active_dialog(ui->d.contrib) != nullptr;
}

bool ui_plugin_dialog_wants_text(const ccord_ui *ui)
{
  if (!ui_plugin_dialog_is_open(ui) || !ui->plugin_v3) return false;
  const v3_dialog_state *d = &ui->plugin_v3->dlg;
  if (d->focused_idx < 0 || (size_t)d->focused_idx >= d->nfields) return false;
  v3_field_kind k = d->fields[d->focused_idx].kind;
  return k == V3F_TEXT || k == V3F_SECRET || k == V3F_COLOR;
}

void ui_plugin_dialog_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui || !r || !ui->d.contrib) return;
  const ccord_plugin_dialog *dlg = ccord_contrib_active_dialog(ui->d.contrib);
  if (!dlg) return;
  v3_dialog_sync(ui, dlg);
  ui_plugin_v3 *v = ui->plugin_v3;
  if (!v) return;

  uis_tokens tk = uis_tokens_of(ui);
  float mw = (dlg->kind == CCORD_DIALOG_FORM) ? UIS_MODAL_W_MD : UIS_MODAL_W_SM;
  ccord_rect probe = uis_modal_box(ui, mw, 220.0f);
  float inner_w = fmaxf(probe.w - UIS_SP_5 * 2.0f, 120.0f);

  ccord_text_style mst = ui_style(ui, CCORD_FONT_REGULAR, tk.fs_control, ui->theme->c.text_normal);
  float msg_h = 0.0f;
  if (dlg->message && dlg->message[0]) {
    ccord_vec2 mm = ccord_text_measure(r, dlg->message, strlen(dlg->message), &mst, inner_w);
    msg_h = mm.y + UIS_SP_3;
  }
  float fields_h = 0.0f;
  for (size_t i = 0; i < v->dlg.nfields; i++) fields_h += v3_field_height(&v->dlg.fields[i]) + UIS_SP_2;

  float mh = 76.0f + UIS_H_CHIP + UIS_SP_3 + msg_h + fields_h + UIS_H_BTN + UIS_SP_5;
  if (mh < 180.0f) mh = 180.0f;
  ccord_rect box = uis_modal_box(ui, mw, mh);
  v->dlg.box_rect = box;
  inner_w = fmaxf(box.w - UIS_SP_5 * 2.0f, 120.0f);

  uis_modal_frame(ui, r, box);
  const char *title = (dlg->title && dlg->title[0]) ? dlg->title : "Plugin Dialog";
  float y = uis_modal_title(ui, r, box, title);

  // Explicit attribution badge so plugins cannot spoof native dialogs.
  char badge[128];
  snprintf(badge, sizeof badge, "Plugin: %s", (dlg->plugin_name && dlg->plugin_name[0]) ? dlg->plugin_name : dlg->owner);
  float bw = fminf(uis_label_width(ui, r, badge), inner_w);
  uis_chip(ui, r, (ccord_rect){box.x + UIS_SP_5, y, bw, UIS_H_CHIP}, badge, (uis_state){.selected = true}, "pdlg_owner", dlg->req_id);
  y += UIS_H_CHIP + UIS_SP_3;

  float footer_top = box.y + box.h - UIS_SP_5 - UIS_H_BTN - UIS_SP_3;
  float body_h = fmaxf(footer_top - y, 36.0f);
  v->dlg.content_h = msg_h + fields_h;
  v->dlg.body_rect = (ccord_rect){box.x + UIS_SP_5, y, inner_w, body_h};
  float max_scroll = v->dlg.content_h > body_h ? (v->dlg.content_h - body_h) : 0.0f;
  if (v->dlg.scroll_y < 0.0f) v->dlg.scroll_y = 0.0f;
  if (v->dlg.scroll_y > max_scroll) v->dlg.scroll_y = max_scroll;

  ccord_push_clip(r, v->dlg.body_rect);
  float cy = y - v->dlg.scroll_y;
  if (dlg->message && dlg->message[0]) {
    ccord_text_draw(r, dlg->message, strlen(dlg->message), box.x + UIS_SP_5, cy, &mst, inner_w);
    cy += msg_h;
  }

  for (size_t i = 0; i < v->dlg.nfields; i++) {
    v3_draw_field(ui, r, &v->dlg.fields[i], i, box.x + UIS_SP_5, cy, inner_w, v->dlg.focused_idx == (int)i, "pdlg_f");
    cy += v3_field_height(&v->dlg.fields[i]) + UIS_SP_2;
  }
  ccord_pop_clip(r);

  const char *ok_lbl = (dlg->confirm_label && dlg->confirm_label[0]) ? dlg->confirm_label : "Confirm";
  const char *cancel_lbl = (dlg->cancel_label && dlg->cancel_label[0]) ? dlg->cancel_label : "Cancel";
  float ok_w = uis_label_width(ui, r, ok_lbl);
  float cancel_w = uis_label_width(ui, r, cancel_lbl);
  if (ok_w < 84.0f) ok_w = 84.0f;
  if (cancel_w < 84.0f) cancel_w = 84.0f;
  if (ok_w + cancel_w + UIS_SP_2 > inner_w) {
    float half = floorf((inner_w - UIS_SP_2) * 0.5f);
    if (half < 56.0f) half = 56.0f;
    ok_w = fminf(ok_w, half);
    cancel_w = fminf(cancel_w, half);
  }

  v->dlg.confirm_rect = uis_modal_footer_button(box, 0, ok_w);
  v->dlg.cancel_rect = (ccord_rect){v->dlg.confirm_rect.x - UIS_SP_2 - cancel_w, v->dlg.confirm_rect.y, cancel_w, UIS_H_BTN};

  uis_button(ui, r, v->dlg.cancel_rect, cancel_lbl, UIS_BTN_SECONDARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, v->dlg.cancel_rect)}, "pdlg_btn", 1);
  uis_button(ui, r, v->dlg.confirm_rect, ok_lbl, UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, v->dlg.confirm_rect)}, "pdlg_btn", 2);
}

bool ui_plugin_dialog_handle_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui || !ev || !ui->d.contrib) return false;
  const ccord_plugin_dialog *dlg = ccord_contrib_active_dialog(ui->d.contrib);
  if (!dlg) return false;
  v3_dialog_sync(ui, dlg);
  ui_plugin_v3 *v = ui->plugin_v3;
  if (!v) return true;

  if (ev->type == CCORD_IN_WHEEL) {
    float max_scroll = v->dlg.content_h > v->dlg.body_rect.h ? (v->dlg.content_h - v->dlg.body_rect.h) : 0.0f;
    v->dlg.scroll_y -= ev->wheel * 32.0f;
    if (v->dlg.scroll_y < 0.0f) v->dlg.scroll_y = 0.0f;
    if (v->dlg.scroll_y > max_scroll) v->dlg.scroll_y = max_scroll;
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    if (v->dlg.drag_slider_idx >= 0 && (size_t)v->dlg.drag_slider_idx < v->dlg.nfields) {
      v3_field *f = &v->dlg.fields[v->dlg.drag_slider_idx];
      f->num_val = v3_slider_from_x(f, ev->x);
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_UP) {
    v->dlg.drag_slider_idx = -1;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (!ui_in_rect(ev->x, ev->y, v->dlg.box_rect) || ui_in_rect(ev->x, ev->y, v->dlg.cancel_rect)) {
      v3_dialog_submit(ui, dlg, false);
      return true;
    }
    if (ui_in_rect(ev->x, ev->y, v->dlg.confirm_rect)) {
      v3_dialog_submit(ui, dlg, true);
      return true;
    }
    v->dlg.focused_idx = -1;
    if (ui->touch_mode) ui->touch_text_focused = false;
    if (ui_in_rect(ev->x, ev->y, v->dlg.body_rect)) {
      for (size_t i = 0; i < v->dlg.nfields; i++) {
        v3_field *f = &v->dlg.fields[i];
        if (!ui_in_rect(ev->x, ev->y, f->hit_rect)) continue;
        if (f->kind == V3F_TOGGLE) {
          f->bool_val = !f->bool_val;
        } else if (f->kind == V3F_SLIDER) {
          f->num_val = v3_slider_from_x(f, ev->x);
          v->dlg.drag_slider_idx = (int)i;
        } else if (f->kind == V3F_SELECT && f->nopts > 0) {
          f->sel_idx = (f->sel_idx + 1u) % f->nopts;
        } else if (f->kind == V3F_TEXT || f->kind == V3F_SECRET || f->kind == V3F_COLOR) {
          v->dlg.focused_idx = (int)i;
          if (ui->touch_mode) ui->touch_text_focused = true;
        }
        ui->dirty = true;
        return true;
      }
    }
    return true;
  }

  if (ev->type == CCORD_IN_KEY_DOWN) {
    if (ev->key == CCORD_KEY_ESCAPE) {
      v3_dialog_submit(ui, dlg, false);
      return true;
    }
    if (ev->key == CCORD_KEY_RETURN && (dlg->kind == CCORD_DIALOG_CONFIRM || dlg->kind == CCORD_DIALOG_PROMPT)) {
      v3_dialog_submit(ui, dlg, true);
      return true;
    }
    if (ev->key == CCORD_KEY_TAB && v->dlg.nfields > 0) {
      int start = v->dlg.focused_idx >= 0 ? v->dlg.focused_idx : 0;
      for (size_t step = 1; step <= v->dlg.nfields; step++) {
        size_t idx = ((size_t)start + step) % v->dlg.nfields;
        v3_field_kind k = v->dlg.fields[idx].kind;
        if (k == V3F_TEXT || k == V3F_SECRET || k == V3F_COLOR) {
          v->dlg.focused_idx = (int)idx;
          ui->dirty = true;
          break;
        }
      }
      return true;
    }
  }

  if (v->dlg.focused_idx >= 0 && (size_t)v->dlg.focused_idx < v->dlg.nfields) {
    v3_field *f = &v->dlg.fields[v->dlg.focused_idx];
    if (v3_handle_text_edit_event(ui, &f->edit, f->kind == V3F_SECRET, ev)) {
      ui->dirty = true;
      return true;
    }
  }
  return true;
}

// ---- Schema-Driven Plugin Settings Modal ----

static void v3_settings_emit(ccord_ui *ui, const char *owner, const v3_field *f)
{
  if (!ui || !ui->d.contrib || !owner || !f || !f->key[0]) return;
  ccord_buf b;
  ccord_buf_init(&b, 0);
  ccord_jw w;
  ccord_jw_init(&w, &b);
  if (f->kind == V3F_TOGGLE) ccord_jw_bool(&w, f->bool_val);
  else if (f->kind == V3F_SLIDER) ccord_jw_double(&w, f->num_val);
  else if (f->kind == V3F_SELECT) {
    const char *sv = (f->nopts > 0 && f->sel_idx < f->nopts) ? f->opts[f->sel_idx].value : "";
    ccord_jw_cstr(&w, sv);
  } else {
    ccord_jw_cstr(&w, ccord_text_edit_cstr(&f->edit));
  }
  if (b.data && ccord_jw_ok(&w)) {
    ccord_contrib_notify_setting_changed(ui->d.contrib, owner, f->key, (const char *)b.data);
  }
  ccord_buf_free(&b);
}

static void v3_settings_sync(ccord_ui *ui)
{
  if (!ui || !ui->plugin_v3 || !ui->plugin_v3->set.open || !ui->d.contrib) return;
  v3_settings_state *s = &ui->plugin_v3->set;
  const ccord_plugin_settings_schema *sc = ccord_contrib_plugin_settings(ui->d.contrib, s->owner);
  if (!sc) {
    s->open = false;
    return;
  }
  if (s->loaded_rev == sc->revision && s->nfields > 0) return;
  v3_parse_field_list(sc->schema_json, sc->values_json, s->fields, &s->nfields);
  s->loaded_rev = sc->revision;
}

void ui_plugin_settings_open(ccord_ui *ui, const char *owner)
{
  if (!ui || !owner || !owner[0] || !ui->d.contrib) return;
  if (!ccord_contrib_plugin_settings(ui->d.contrib, owner)) return;
  ui_plugin_v3 *v = pv3_v3_ensure(ui);
  if (!v) return;
  v->set.open = true;
  snprintf(v->set.owner, sizeof v->set.owner, "%s", owner);
  snprintf(v->set.plugin_name, sizeof v->set.plugin_name, "%s", owner);
  size_t np = ccord_contrib_plugin_count(ui->d.contrib);
  for (size_t i = 0; i < np; i++) {
    const ccord_plugin_info *p = ccord_contrib_plugin_at(ui->d.contrib, i);
    if (p && p->id && strcmp(p->id, owner) == 0 && p->name && p->name[0]) {
      snprintf(v->set.plugin_name, sizeof v->set.plugin_name, "%s", p->name);
      break;
    }
  }
  v->set.loaded_rev = 0;
  v->set.focused_idx = -1;
  v->set.drag_slider_idx = -1;
  v->set.scroll_y = 0.0f;
  v3_settings_sync(ui);
  ui->dirty = true;
}

bool ui_plugin_settings_is_open(const ccord_ui *ui)
{
  return ui && ui->plugin_v3 && ui->plugin_v3->set.open;
}

bool ui_plugin_settings_wants_text(const ccord_ui *ui)
{
  if (!ui_plugin_settings_is_open(ui)) return false;
  const v3_settings_state *s = &ui->plugin_v3->set;
  if (s->focused_idx < 0 || (size_t)s->focused_idx >= s->nfields) return false;
  v3_field_kind k = s->fields[s->focused_idx].kind;
  return k == V3F_TEXT || k == V3F_SECRET || k == V3F_COLOR;
}

void ui_plugin_settings_draw(ccord_ui *ui, ccord_renderer *r)
{
  if (!ui_plugin_settings_is_open(ui) || !r) return;
  v3_settings_sync(ui);
  v3_settings_state *s = &ui->plugin_v3->set;
  if (!s->open) return;

  float content_h = 0.0f;
  for (size_t i = 0; i < s->nfields; i++) content_h += v3_field_height(&s->fields[i]) + UIS_SP_2;
  s->content_h = content_h;

  float desired_body = content_h < 80.0f ? 80.0f : content_h;
  float mh = 76.0f + UIS_H_CHIP + UIS_SP_3 + desired_body + UIS_H_BTN + UIS_SP_5;
  ccord_rect box = uis_modal_box(ui, UIS_MODAL_W_MD, mh);
  s->box_rect = box;

  uis_modal_frame(ui, r, box);
  char title[128];
  snprintf(title, sizeof title, "%s Settings", s->plugin_name);
  float y = uis_modal_title(ui, r, box, title);

  float inner_w = fmaxf(box.w - UIS_SP_5 * 2.0f, 120.0f);
  float bw = fminf(uis_label_width(ui, r, s->owner), inner_w);
  uis_chip(ui, r, (ccord_rect){box.x + UIS_SP_5, y, bw, UIS_H_CHIP}, s->owner, (uis_state){0}, "pset_id", ui_name_id(s->owner));
  y += UIS_H_CHIP + UIS_SP_3;

  float footer_top = box.y + box.h - UIS_SP_5 - UIS_H_BTN - UIS_SP_3;
  float body_h = fmaxf(footer_top - y, 36.0f);
  ccord_rect body = {box.x + UIS_SP_5, y, inner_w, body_h};
  s->body_rect = body;

  float max_scroll = content_h > body_h ? (content_h - body_h) : 0.0f;
  if (s->scroll_y < 0.0f) s->scroll_y = 0.0f;
  if (s->scroll_y > max_scroll) s->scroll_y = max_scroll;

  ccord_push_clip(r, body);
  float fy = body.y - s->scroll_y;
  for (size_t i = 0; i < s->nfields; i++) {
    v3_draw_field(ui, r, &s->fields[i], i, body.x, fy, inner_w, s->focused_idx == (int)i, "pset_f");
    fy += v3_field_height(&s->fields[i]) + UIS_SP_2;
  }
  ccord_pop_clip(r);

  float done_w = uis_label_width(ui, r, "Done");
  if (done_w < 84.0f) done_w = 84.0f;
  s->done_rect = uis_modal_footer_button(box, 0, done_w);
  uis_button(ui, r, s->done_rect, "Done", UIS_BTN_PRIMARY, UIS_BTN_S_MD,
             (uis_state){.hover = ui_in_rect(ui->mx, ui->my, s->done_rect)}, "pset_done", 1);
}

bool ui_plugin_settings_handle_input(ccord_ui *ui, const ccord_input_event *ev)
{
  if (!ui_plugin_settings_is_open(ui) || !ev) return false;
  v3_settings_state *s = &ui->plugin_v3->set;

  if (ev->type == CCORD_IN_WHEEL) {
    float max_scroll = s->content_h > s->body_rect.h ? (s->content_h - s->body_rect.h) : 0.0f;
    s->scroll_y -= ev->wheel * 32.0f;
    if (s->scroll_y < 0.0f) s->scroll_y = 0.0f;
    if (s->scroll_y > max_scroll) s->scroll_y = max_scroll;
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_MOVE) {
    if (s->drag_slider_idx >= 0 && (size_t)s->drag_slider_idx < s->nfields) {
      v3_field *f = &s->fields[s->drag_slider_idx];
      f->num_val = v3_slider_from_x(f, ev->x);
      v3_settings_emit(ui, s->owner, f);
      ui->dirty = true;
    }
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_UP) {
    s->drag_slider_idx = -1;
    return true;
  }
  if (ev->type == CCORD_IN_MOUSE_DOWN && ev->button == 1) {
    if (!ui_in_rect(ev->x, ev->y, s->box_rect) || ui_in_rect(ev->x, ev->y, s->done_rect)) {
      if (s->focused_idx >= 0 && (size_t)s->focused_idx < s->nfields) {
        v3_settings_emit(ui, s->owner, &s->fields[s->focused_idx]);
      }
      s->open = false;
      if (ui->touch_mode) ui->touch_text_focused = false;
      ui->dirty = true;
      return true;
    }
    int prev_focus = s->focused_idx;
    s->focused_idx = -1;
    if (ui->touch_mode) ui->touch_text_focused = false;
    if (ui_in_rect(ev->x, ev->y, s->body_rect)) {
      for (size_t i = 0; i < s->nfields; i++) {
        v3_field *f = &s->fields[i];
        if (!ui_in_rect(ev->x, ev->y, f->hit_rect)) continue;
        if (f->kind == V3F_TOGGLE) {
          f->bool_val = !f->bool_val;
          v3_settings_emit(ui, s->owner, f);
        } else if (f->kind == V3F_SLIDER) {
          f->num_val = v3_slider_from_x(f, ev->x);
          s->drag_slider_idx = (int)i;
          v3_settings_emit(ui, s->owner, f);
        } else if (f->kind == V3F_SELECT && f->nopts > 0) {
          f->sel_idx = (f->sel_idx + 1u) % f->nopts;
          v3_settings_emit(ui, s->owner, f);
        } else if (f->kind == V3F_TEXT || f->kind == V3F_SECRET || f->kind == V3F_COLOR) {
          s->focused_idx = (int)i;
          if (ui->touch_mode) ui->touch_text_focused = true;
        }
        break;
      }
    }
    if (prev_focus >= 0 && prev_focus != s->focused_idx && (size_t)prev_focus < s->nfields) {
      v3_settings_emit(ui, s->owner, &s->fields[prev_focus]);
    }
    ui->dirty = true;
    return true;
  }
  if (ev->type == CCORD_IN_KEY_DOWN && ev->key == CCORD_KEY_ESCAPE) {
    if (s->focused_idx >= 0 && (size_t)s->focused_idx < s->nfields) {
      v3_settings_emit(ui, s->owner, &s->fields[s->focused_idx]);
    }
    s->open = false;
    ui->dirty = true;
    return true;
  }
  if (s->focused_idx >= 0 && (size_t)s->focused_idx < s->nfields) {
    v3_field *f = &s->fields[s->focused_idx];
    if (ev->type == CCORD_IN_KEY_DOWN && ev->key == CCORD_KEY_RETURN) {
      v3_settings_emit(ui, s->owner, f);
      ui->dirty = true;
      return true;
    }
    if (v3_handle_text_edit_event(ui, &f->edit, f->kind == V3F_SECRET, ev)) {
      v3_settings_emit(ui, s->owner, f);
      ui->dirty = true;
      return true;
    }
  }
  return true;
}

