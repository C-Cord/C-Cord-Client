// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include <ccord/model/report.h>

#include <ccord/core/alloc.h>
#include <ccord/core/json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPORT_MAX_ELEMENTS 64
#define REPORT_MAX_VALUE 4000

const char *ccord_report_type_name(ccord_report_type t)
{
  switch (t) {
    case CCORD_REPORT_MESSAGE: return "message";
    case CCORD_REPORT_USER: return "user";
    default: return nullptr;
  }
}

// Null-safe views of the JSON accessors (a missing member is a null node).
static ccord_json_type rj_type_of(const ccord_json *j) { return j ? ccord_json_type_of(j) : CCORD_JSON_NULL; }
static bool rj_bool(const ccord_json *j, bool f) { return j ? ccord_json_bool(j, f) : f; }
static size_t rj_array_len(const ccord_json *j) { return j ? ccord_json_array_len(j) : 0; }
static const ccord_json *rj_array_at(const ccord_json *j, size_t i) { return j ? ccord_json_array_at(j, i) : nullptr; }
static size_t rj_object_len(const ccord_json *j) { return j ? ccord_json_object_len(j) : 0; }
static const ccord_json *rj_get(const ccord_json *o, const char *k) { return o ? ccord_json_get(o, k) : nullptr; }
static bool rj_int64(const ccord_json *j, int64_t *out) { return j && ccord_json_int64(j, out); }
static const char *rj_string(const ccord_json *j, size_t *n) { return j ? ccord_json_string(j, n) : nullptr; }

// ---- parsing ---------------------------------------------------------------------------------------------

static const char *jstr(const ccord_json *o, const char *key)
{
  size_t n;
  const ccord_json *v = o ? rj_get(o, key) : nullptr;
  return v ? rj_string(v, &n) : nullptr;
}

// Copies a string into the menu arena; nullptr when absent or not a string.
static const char *dup_str(ccord_arena *a, const ccord_json *j, bool *oom)
{
  size_t n;
  const char *s = j ? rj_string(j, &n) : nullptr;
  if (!s) return nullptr;
  char *d = ccord_arena_strndup(a, s, n);
  if (!d) *oom = true;
  return d;
}

static const char *dup_key(ccord_arena *a, const ccord_json *o, const char *key, bool *oom)
{
  return o ? dup_str(a, rj_get(o, key), oom) : nullptr;
}

static bool json_u32(const ccord_json *j, uint32_t *out)
{
  int64_t v;
  if (!j || !rj_int64(j, &v) || v < 0 || v > UINT32_MAX) return false;
  *out = (uint32_t)v;
  return true;
}

static void copy_field(char *dst, size_t cap, const ccord_json *o, const char *key)
{
  const char *s = jstr(o, key);
  if (!s) { dst[0] = '\0'; return; }
  snprintf(dst, cap, "%s", s);
}

static ccord_report_element_kind kind_of(const char *t)
{
  static const struct { const char *name; ccord_report_element_kind k; } tbl[] = {
    {"checkbox", CCORD_REPORT_EL_CHECKBOX}, {"dropdown", CCORD_REPORT_EL_DROPDOWN},
    {"free_text", CCORD_REPORT_EL_FREE_TEXT}, {"text", CCORD_REPORT_EL_TEXT},
    {"external_link", CCORD_REPORT_EL_EXTERNAL_LINK}, {"breadcrumbs", CCORD_REPORT_EL_BREADCRUMBS},
    {"success", CCORD_REPORT_EL_SUCCESS}, {"fail", CCORD_REPORT_EL_FAIL},
    {"block_users", CCORD_REPORT_EL_BLOCK_USERS}, {"message_preview", CCORD_REPORT_EL_MESSAGE_PREVIEW},
    {"user_preview", CCORD_REPORT_EL_USER_PREVIEW},
  };
  if (t) {
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
      if (strcmp(t, tbl[i].name) == 0) return tbl[i].k;
    }
  }
  return CCORD_REPORT_EL_UNKNOWN;
}

static ccord_status parse_choices(ccord_arena *a, ccord_report_element *el, const ccord_json *arr, bool dropdown, bool *oom)
{
  size_t n = rj_array_len(arr);
  if (n == 0) return CCORD_OK;
  ccord_report_choice *c = ccord_arena_alloc(a, n * sizeof *c, alignof(ccord_report_choice));
  if (!c) { *oom = true; return CCORD_ERR_NOMEM; }
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    const ccord_json *row = rj_array_at(arr, i);
    ccord_report_choice ch = {0};
    if (dropdown) {
      ch.value = dup_key(a, row, "value", oom);
      ch.label = dup_key(a, row, "label", oom);
      ch.description = dup_key(a, row, "description", oom);
    } else {   // checkbox row: [name, label, description?]
      ch.value = dup_str(a, rj_array_at(row, 0), oom);
      ch.label = dup_str(a, rj_array_at(row, 1), oom);
      ch.description = dup_str(a, rj_array_at(row, 2), oom);
    }
    if (*oom) return CCORD_ERR_NOMEM;
    if (!ch.value) continue;
    if (!ch.label) ch.label = ch.value;
    c[k++] = ch;
  }
  el->choices = c;
  el->choice_count = k;
  return CCORD_OK;
}

static ccord_status parse_element(ccord_arena *a, ccord_report_element *el, const ccord_json *j, bool *oom)
{
  const char *type = jstr(j, "type");
  el->kind = kind_of(type);
  el->name = dup_key(a, j, "name", oom);
  el->should_submit = rj_bool(rj_get(j, "should_submit_data"), false);
  const ccord_json *d = rj_get(j, "data");
  if (d && rj_type_of(d) == CCORD_JSON_OBJECT) {
    el->title = dup_key(a, d, "title", oom);
    el->subtitle = dup_key(a, d, "subtitle", oom);
    el->placeholder = dup_key(a, d, "placeholder", oom);
    el->body = dup_key(a, d, "body", oom);
    if (!el->title) el->title = dup_key(a, d, "header", oom);   // text element
    el->url = dup_key(a, d, "url", oom);
    el->link_text = dup_key(a, d, "link_text", oom);
    (void)json_u32(rj_get(d, "rows"), &el->rows);
    (void)json_u32(rj_get(d, "character_limit"), &el->character_limit);
    if (el->kind == CCORD_REPORT_EL_DROPDOWN) {
      ccord_status s = parse_choices(a, el, rj_get(d, "options"), true, oom);
      if (s != CCORD_OK) return s;
    }
  } else if (d && rj_type_of(d) == CCORD_JSON_ARRAY && el->kind == CCORD_REPORT_EL_CHECKBOX) {
    ccord_status s = parse_choices(a, el, d, false, oom);
    if (s != CCORD_OK) return s;
  }
  if (el->kind == CCORD_REPORT_EL_FREE_TEXT && el->character_limit == 0) el->character_limit = 800;
  return *oom ? CCORD_ERR_NOMEM : CCORD_OK;
}

static ccord_report_button button_of(const char *t)
{
  if (!t) return CCORD_REPORT_BTN_NONE;
  if (strcmp(t, "next") == 0) return CCORD_REPORT_BTN_NEXT;
  if (strcmp(t, "submit") == 0) return CCORD_REPORT_BTN_SUBMIT;
  if (strcmp(t, "done") == 0) return CCORD_REPORT_BTN_DONE;
  if (strcmp(t, "cancel") == 0) return CCORD_REPORT_BTN_CANCEL;
  return CCORD_REPORT_BTN_NONE;
}

// Fills *nd from j. Returns CCORD_OK, CCORD_ERR_NOMEM, or CCORD_ERR_INVALID when the node must be dropped.
static ccord_status parse_node(ccord_arena *a, ccord_report_node *nd, const char *key, const ccord_json *j, bool *oom)
{
  if (rj_type_of(j) != CCORD_JSON_OBJECT) return CCORD_ERR_INVALID;
  uint32_t id;
  if (!json_u32(rj_get(j, "id"), &id)) return CCORD_ERR_INVALID;
  char *end;
  unsigned long kv = strtoul(key, &end, 10);
  if (end == key || *end != '\0' || kv != id) return CCORD_ERR_INVALID;   // the key must equal node.id
  nd->id = id;
  nd->key = dup_key(a, j, "key", oom);
  nd->header = dup_key(a, j, "header", oom);
  nd->subheader = dup_key(a, j, "subheader", oom);
  nd->info = dup_key(a, j, "info", oom);
  nd->report_type = dup_key(a, j, "report_type", oom);
  nd->multi_select_required = rj_bool(rj_get(j, "is_multi_select_required"), false);
  nd->auto_submit = rj_bool(rj_get(j, "is_auto_submit"), false);
  if (*oom) return CCORD_ERR_NOMEM;

  const ccord_json *btn = rj_get(j, "button");
  if (btn && rj_type_of(btn) == CCORD_JSON_OBJECT) {
    nd->button = button_of(jstr(btn, "type"));
    nd->has_button_target = json_u32(rj_get(btn, "target"), &nd->button_target);
  }

  const ccord_json *ch = rj_get(j, "children");
  size_t cn = rj_array_len(ch);
  if (cn) {
    ccord_report_child *c = ccord_arena_alloc(a, cn * sizeof *c, alignof(ccord_report_child));
    if (!c) { *oom = true; return CCORD_ERR_NOMEM; }
    size_t k = 0;
    for (size_t i = 0; i < cn; i++) {
      const ccord_json *e = rj_array_at(ch, i);
      const ccord_json *nm, *tg;
      if (rj_type_of(e) == CCORD_JSON_ARRAY) {
        nm = rj_array_at(e, 0);
        tg = rj_array_at(e, 1);
      } else {
        nm = rj_get(e, "name");
        tg = rj_get(e, "target_node_id");
      }
      ccord_report_child rc = {0};
      if (!json_u32(tg, &rc.target)) continue;
      rc.name = dup_str(a, nm, oom);
      if (*oom) return CCORD_ERR_NOMEM;
      if (!rc.name) rc.name = "";
      c[k++] = rc;
    }
    nd->children = c;
    nd->child_count = k;
  }

  const ccord_json *els = rj_get(j, "elements");
  size_t en = rj_array_len(els);
  if (en) {
    ccord_report_element *e = ccord_arena_alloc(a, en * sizeof *e, alignof(ccord_report_element));
    if (!e) { *oom = true; return CCORD_ERR_NOMEM; }
    size_t k = 0;
    for (size_t i = 0; i < en; i++) {
      const ccord_json *ej = rj_array_at(els, i);
      if (rj_type_of(ej) != CCORD_JSON_OBJECT) continue;
      ccord_report_element el = {0};
      ccord_status s = parse_element(a, &el, ej, oom);
      if (s != CCORD_OK) return s;
      e[k++] = el;
    }
    nd->elements = e;
    nd->element_count = k;
  }
  return CCORD_OK;
}

ccord_status ccord_report_menu_parse(ccord_report_menu *out, const char *json, size_t len, ccord_error *err)
{
  if (!out) return ccord_error_set(err, CCORD_ERR_INVALID, "report menu: null output");
  memset(out, 0, sizeof *out);
  if (!json) return ccord_error_set(err, CCORD_ERR_INVALID, "report menu: no input");

  ccord_arena tmp;
  ccord_arena_init(&tmp, 64 * 1024);
  // A real menu is large: the default 8192-node limit is too small.
  const ccord_json_limits lim = {.max_depth = 16, .max_nodes = 262144, .max_string = 1u << 20};
  const ccord_json *root = ccord_json_parse(&tmp, json, len, &lim, err);
  ccord_status st = CCORD_OK;
  bool oom = false;
  if (!root) {
    st = err && err->code != CCORD_OK ? err->code : CCORD_ERR_PARSE;
    goto done;
  }
  const ccord_json *nodes = rj_get(root, "nodes");
  if (rj_type_of(root) != CCORD_JSON_OBJECT || !nodes || rj_type_of(nodes) != CCORD_JSON_OBJECT ||
      !json_u32(rj_get(root, "root_node_id"), &out->root)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "report menu: missing nodes or root_node_id");
    goto done;
  }
  (void)json_u32(rj_get(root, "success_node_id"), &out->success);
  (void)json_u32(rj_get(root, "fail_node_id"), &out->fail);
  copy_field(out->name, sizeof out->name, root, "name");
  copy_field(out->version, sizeof out->version, root, "version");
  copy_field(out->variant, sizeof out->variant, root, "variant");
  copy_field(out->language, sizeof out->language, root, "language");

  ccord_arena_init(&out->arena, 0);
  size_t n = rj_object_len(nodes);
  if (n) {
    out->nodes = ccord_arena_alloc(&out->arena, n * sizeof *out->nodes, alignof(ccord_report_node));
    if (!out->nodes) { oom = true; goto done; }
  }
  for (size_t i = 0; i < n; i++) {
    const ccord_json *v;
    const char *key = ccord_json_object_key(nodes, i, &v);
    ccord_report_node nd = {0};
    ccord_status s = parse_node(&out->arena, &nd, key, v, &oom);
    if (oom) goto done;
    if (s == CCORD_OK) out->nodes[out->node_count++] = nd;
  }
  if (!ccord_report_menu_node(out, out->root)) {
    st = ccord_error_set(err, CCORD_ERR_PARSE, "report menu: root node %u is missing", out->root);
    goto done;
  }
done:
  if (oom) st = ccord_error_set(err, CCORD_ERR_NOMEM, "report menu: out of memory");
  ccord_arena_free(&tmp);
  if (st != CCORD_OK) ccord_report_menu_free(out);
  return st;
}

const ccord_report_node *ccord_report_menu_node(const ccord_report_menu *m, uint32_t id)
{
  if (!m) return nullptr;
  for (size_t i = 0; i < m->node_count; i++) {
    if (m->nodes[i].id == id) return &m->nodes[i];
  }
  return nullptr;
}

void ccord_report_menu_free(ccord_report_menu *m)
{
  if (!m) return;
  ccord_arena_free(&m->arena);
  memset(m, 0, sizeof *m);
}

// ---- body ------------------------------------------------------------------------------------------------

static bool valid_text(const char *s)
{
  return s && strlen(s) <= REPORT_MAX_VALUE;
}

ccord_status ccord_report_build_body(const ccord_report_request *req, ccord_buf *out, ccord_error *err)
{
  if (!req || !out) return ccord_error_set(err, CCORD_ERR_INVALID, "report body: null argument");
  const char *type = ccord_report_type_name(req->type);
  if (!type) return ccord_error_set(err, CCORD_ERR_INVALID, "report body: unknown report type");
  if (req->type == CCORD_REPORT_MESSAGE && (!req->channel_id || !req->message_id)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "report body: a message report needs channel and message ids");
  }
  if (req->type == CCORD_REPORT_USER && !req->reported_user_id) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "report body: a user report needs the reported user id");
  }
  if (!req->breadcrumbs || req->breadcrumb_count < 1 || req->breadcrumb_count > CCORD_REPORT_MAX_PATH) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "report body: path must hold 1..%d nodes", CCORD_REPORT_MAX_PATH);
  }
  if (req->element_count > REPORT_MAX_ELEMENTS || (req->element_count && !req->elements)) {
    return ccord_error_set(err, CCORD_ERR_RANGE, "report body: too many elements");
  }
  if (!req->version || !req->variant) return ccord_error_set(err, CCORD_ERR_INVALID, "report body: version and variant required");
  for (size_t i = 0; i < req->element_count; i++) {
    const ccord_report_value *v = &req->elements[i];
    if (!valid_text(v->name) || (v->value_count && !v->values)) {
      return ccord_error_set(err, CCORD_ERR_INVALID, "report body: bad element name");
    }
    for (size_t k = 0; k < v->value_count; k++) {
      if (!valid_text(v->values[k])) return ccord_error_set(err, CCORD_ERR_INVALID, "report body: bad element value");
    }
  }

  ccord_jw w;
  ccord_jw_init(&w, out);
  ccord_jw_begin_object(&w);
  ccord_jw_key(&w, "version");  ccord_jw_cstr(&w, req->version);
  ccord_jw_key(&w, "variant");  ccord_jw_cstr(&w, req->variant);
  ccord_jw_key(&w, "name");     ccord_jw_cstr(&w, type);
  ccord_jw_key(&w, "language"); ccord_jw_cstr(&w, req->language && req->language[0] ? req->language : "en");
  // U1 (unverified): breadcrumbs start with the menu's root_node_id. The caller passes the whole path
  // [root, ..., node whose Submit was pressed]; it is sent as given.
  ccord_jw_key(&w, "breadcrumbs");
  ccord_jw_begin_array(&w);
  for (size_t i = 0; i < req->breadcrumb_count; i++) ccord_jw_int(&w, req->breadcrumbs[i]);
  ccord_jw_end_array(&w);
  // U2 (unverified): every elements value is an array of strings, even for a dropdown or free text.
  ccord_jw_key(&w, "elements");
  ccord_jw_begin_object(&w);
  for (size_t i = 0; i < req->element_count; i++) {
    const ccord_report_value *v = &req->elements[i];
    ccord_jw_key(&w, v->name);
    ccord_jw_begin_array(&w);
    for (size_t k = 0; k < v->value_count; k++) ccord_jw_cstr(&w, v->values[k]);
    ccord_jw_end_array(&w);
  }
  ccord_jw_end_object(&w);
  if (req->type == CCORD_REPORT_MESSAGE) {
    // U3 (unverified): a message report sends only channel_id + message_id, no guild_id.
    ccord_jw_key(&w, "channel_id"); ccord_jw_id(&w, req->channel_id);
    ccord_jw_key(&w, "message_id"); ccord_jw_id(&w, req->message_id);
  } else {
    ccord_jw_key(&w, "reported_user_id"); ccord_jw_id(&w, req->reported_user_id);
  }
  ccord_jw_end_object(&w);
  if (!ccord_jw_ok(&w)) {
    ccord_buf_clear(out);
    return ccord_error_set(err, CCORD_ERR_INVALID, "report body: could not encode (invalid UTF-8 or too large)");
  }
  return CCORD_OK;
}
