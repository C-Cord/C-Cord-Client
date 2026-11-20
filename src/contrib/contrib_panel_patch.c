// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#include "contrib_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ccord/core/alloc.h>
#include <ccord/core/buf.h>
#include <ccord/core/json.h>
#include <ccord/core/utf8.h>

// ---- JSON patch writer for ui.panel.patch ----
static void jw_emit_val(ccord_jw *w, const ccord_json *j);
static void jw_emit_val(ccord_jw *w, const ccord_json *j)
{
  if (!j) { ccord_jw_null(w); return; }
  switch (ccord_json_type_of(j)) {
    case CCORD_JSON_NULL: ccord_jw_null(w); break;
    case CCORD_JSON_BOOL: ccord_jw_bool(w, ccord_json_bool(j, false)); break;
    case CCORD_JSON_NUMBER: ccord_jw_double(w, ccord_json_number(j, 0.0)); break;
    case CCORD_JSON_STRING: {
      size_t n = 0;
      const char *s = ccord_json_string(j, &n);
      ccord_jw_string(w, s ? s : "", n);
      break;
    }
    case CCORD_JSON_ARRAY: {
      ccord_jw_begin_array(w);
      for (size_t i = 0; i < ccord_json_array_len(j); i++) jw_emit_val(w, ccord_json_array_at(j, i));
      ccord_jw_end_array(w);
      break;
    }
    case CCORD_JSON_OBJECT: {
      ccord_jw_begin_object(w);
      for (size_t i = 0; i < ccord_json_object_len(j); i++) {
        const ccord_json *v = nullptr;
        const char *k = ccord_json_object_key(j, i, &v);
        ccord_jw_key(w, k);
        jw_emit_val(w, v);
      }
      ccord_jw_end_object(w);
      break;
    }
  }
}

typedef struct patch_op {
  const char *op;
  const char *target_id;
  const char *prop;
  const ccord_json *value;
  const ccord_json *node;
  const ccord_json *props;
} patch_op;

static bool jw_emit_patched(ccord_jw *w, const ccord_json *node, const patch_op *ops, size_t nops, bool *matched)
{
  if (!node || ccord_json_type_of(node) != CCORD_JSON_OBJECT) {
    jw_emit_val(w, node);
    return true;
  }
  size_t idl = 0;
  const char *nid = ccord_json_string(ccord_json_get(node, "id"), &idl);
  // Check remove / replace ops first
  if (nid && idl > 0) {
    for (size_t k = 0; k < nops; k++) {
      if (!ops[k].target_id || strcmp(ops[k].target_id, nid) != 0) continue;
      if (strcmp(ops[k].op, "remove") == 0) {
        *matched = true;
        return false;   // omit from parent's children array
      }
      if (strcmp(ops[k].op, "replace") == 0 && ops[k].node) {
        *matched = true;
        jw_emit_val(w, ops[k].node);
        return true;
      }
    }
  }
  ccord_jw_begin_object(w);
  size_t nkeys = ccord_json_object_len(node);
  bool children_written = false;
  for (size_t i = 0; i < nkeys; i++) {
    const ccord_json *v = nullptr;
    const char *k = ccord_json_object_key(node, i, &v);
    // Check if any op overrides this key on this node
    const ccord_json *override_val = nullptr;
    if (nid && idl > 0) {
      for (size_t o = 0; o < nops; o++) {
        if (!ops[o].target_id || strcmp(ops[o].target_id, nid) != 0) continue;
        if (strcmp(ops[o].op, "set_text") == 0 && strcmp(k, "text") == 0 && ops[o].value) {
          override_val = ops[o].value; *matched = true;
        } else if (strcmp(ops[o].op, "set_prop") == 0 && ops[o].prop && strcmp(k, ops[o].prop) == 0) {
          override_val = ops[o].value; *matched = true;
        } else if (strcmp(ops[o].op, "set_props") == 0 && ops[o].props &&
                   ccord_json_type_of(ops[o].props) == CCORD_JSON_OBJECT) {
          const ccord_json *pv = ccord_json_get(ops[o].props, k);
          if (pv) { override_val = pv; *matched = true; }
        }
      }
    }
    if (strcmp(k, "children") == 0 && !override_val && v && ccord_json_type_of(v) == CCORD_JSON_ARRAY) {
      children_written = true;
      ccord_jw_key(w, "children");
      ccord_jw_begin_array(w);
      for (size_t ci = 0; ci < ccord_json_array_len(v); ci++) {
        const ccord_json *ch = ccord_json_array_at(v, ci);
        // Check if child is removed before emitting
        size_t cidl = 0;
        const char *cid = (ch && ccord_json_type_of(ch) == CCORD_JSON_OBJECT)
                            ? ccord_json_string(ccord_json_get(ch, "id"), &cidl)
                            : nullptr;
        bool skip = false;
        if (cid && cidl > 0) {
          for (size_t o = 0; o < nops; o++) {
            if (ops[o].target_id && strcmp(ops[o].target_id, cid) == 0 && strcmp(ops[o].op, "remove") == 0) {
              skip = true; *matched = true; break;
            }
          }
        }
        if (!skip) (void)jw_emit_patched(w, ch, ops, nops, matched);
      }
      // Append children targeting this node
      if (nid && idl > 0) {
        for (size_t o = 0; o < nops; o++) {
          if (ops[o].target_id && strcmp(ops[o].target_id, nid) == 0 &&
              strcmp(ops[o].op, "append") == 0 && ops[o].node) {
            jw_emit_val(w, ops[o].node);
            *matched = true;
          }
        }
      }
      ccord_jw_end_array(w);
    } else {
      ccord_jw_key(w, k);
      jw_emit_val(w, override_val ? override_val : v);
    }
  }
  // Emit newly added properties that did not exist on `node`
  if (nid && idl > 0) {
    for (size_t o = 0; o < nops; o++) {
      if (!ops[o].target_id || strcmp(ops[o].target_id, nid) != 0) continue;
      if (strcmp(ops[o].op, "set_text") == 0 && ops[o].value && !ccord_json_get(node, "text")) {
        ccord_jw_key(w, "text"); jw_emit_val(w, ops[o].value); *matched = true;
      } else if (strcmp(ops[o].op, "set_prop") == 0 && ops[o].prop && !ccord_json_get(node, ops[o].prop)) {
        ccord_jw_key(w, ops[o].prop); jw_emit_val(w, ops[o].value); *matched = true;
      } else if (strcmp(ops[o].op, "set_props") == 0 && ops[o].props &&
                 ccord_json_type_of(ops[o].props) == CCORD_JSON_OBJECT) {
        for (size_t pi = 0; pi < ccord_json_object_len(ops[o].props); pi++) {
          const ccord_json *pv = nullptr;
          const char *pk = ccord_json_object_key(ops[o].props, pi, &pv);
          if (pk && !ccord_json_get(node, pk)) {
            ccord_jw_key(w, pk); jw_emit_val(w, pv); *matched = true;
          }
        }
      } else if (strcmp(ops[o].op, "append") == 0 && ops[o].node && !children_written) {
        children_written = true;
        ccord_jw_key(w, "children");
        ccord_jw_begin_array(w);
        jw_emit_val(w, ops[o].node);
        ccord_jw_end_array(w);
        *matched = true;
      }
    }
  }
  ccord_jw_end_object(w);
  return true;
}

ccord_status ccord_contrib_patch_panel(ccord_contrib *c, const char *owner, const char *panel_id,
                                       const char *patch_json, ccord_error *err)
{
  ccord_error_clear(err);
  if (!c || !owner || !panel_id || !patch_json) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: bad panel patch");
  }
  ccord_panel_contribution *p = nullptr;
  for (size_t i = 0; i < c->npanels; i++) {
    if (strcmp(c->panels[i].owner, owner) == 0 && strcmp(c->panels[i].panel_id, panel_id) == 0) {
      p = &c->panels[i];
      break;
    }
  }
  if (!p) return ccord_error_set(err, CCORD_ERR_NOT_FOUND, "contrib: panel not found");

  static const ccord_json_limits k_lim = {32, 8192, CONTRIB_PANEL_JSON_MAX};
  ccord_arena ar;
  ccord_arena_init(&ar, 4096u);
  ccord_error pe = {0};
  const ccord_json *root = ccord_json_parse(&ar, p->root_json, strlen(p->root_json), &k_lim, &pe);
  const ccord_json *pj = ccord_json_parse(&ar, patch_json, strlen(patch_json), &k_lim, &pe);
  if (!root || !pj) {
    ccord_arena_free(&ar);
    return ccord_error_set(err, CCORD_ERR_PARSE, "contrib: invalid panel patch JSON");
  }
  patch_op ops[64];
  size_t nops = 0;
  if (ccord_json_type_of(pj) == CCORD_JSON_ARRAY) {
    size_t n = ccord_json_array_len(pj);
    for (size_t i = 0; i < n && nops < 64u; i++) {
      const ccord_json *item = ccord_json_array_at(pj, i);
      if (!item || ccord_json_type_of(item) != CCORD_JSON_OBJECT) continue;
      size_t ol = 0, tl = 0, pl = 0;
      const char *op = ccord_json_string(ccord_json_get(item, "op"), &ol);
      const char *tid = ccord_json_string(ccord_json_get(item, "target_id"), &tl);
      if (!tid) tid = ccord_json_string(ccord_json_get(item, "id"), &tl);
      if (!op || !tid) continue;
      ops[nops++] = (patch_op){
        .op = op,
        .target_id = tid,
        .prop = ccord_json_string(ccord_json_get(item, "prop"), &pl),
        .value = ccord_json_get(item, "value"),
        .node = ccord_json_get(item, "node"),
        .props = ccord_json_get(item, "props"),
      };
    }
  } else if (ccord_json_type_of(pj) == CCORD_JSON_OBJECT) {
    size_t ol = 0, tl = 0, pl = 0;
    const char *op = ccord_json_string(ccord_json_get(pj, "op"), &ol);
    const char *tid = ccord_json_string(ccord_json_get(pj, "target_id"), &tl);
    if (!tid) tid = ccord_json_string(ccord_json_get(pj, "id"), &tl);
    if (op && tid) {
      ops[nops++] = (patch_op){
        .op = op,
        .target_id = tid,
        .prop = ccord_json_string(ccord_json_get(pj, "prop"), &pl),
        .value = ccord_json_get(pj, "value"),
        .node = ccord_json_get(pj, "node"),
        .props = ccord_json_get(pj, "props"),
      };
    }
  }
  if (nops == 0) {
    ccord_arena_free(&ar);
    return ccord_error_set(err, CCORD_ERR_INVALID, "contrib: empty panel patch");
  }
  ccord_buf out;
  ccord_buf_init(&out, CONTRIB_PANEL_JSON_MAX);
  ccord_jw w;
  ccord_jw_init(&w, &out);
  bool matched = false;
  (void)jw_emit_patched(&w, root, ops, nops, &matched);
  ccord_arena_free(&ar);
  if (!ccord_jw_ok(&w) || out.len == 0 || out.len > CONTRIB_PANEL_JSON_MAX) {
    ccord_buf_free(&out);
    return ccord_error_set(err, CCORD_ERR_RANGE, "contrib: patched panel JSON invalid or too large");
  }
  char *fresh = ccord_strndup(ccord_buf_cstr(&out), out.len);
  ccord_buf_free(&out);
  if (!fresh) return ccord_error_set(err, CCORD_ERR_NOMEM, "contrib: out of memory");
  contrib_free_str(p->root_json);
  p->root_json = fresh;
  p->revision = ++c->next_panel_rev;
  c->revision++;
  return CCORD_OK;
}

