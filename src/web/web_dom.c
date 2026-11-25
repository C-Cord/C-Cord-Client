// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "web_internal.h"

#include <ccord/core/os.h>
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Helpers ----

static char *dup_lower(const char *s, size_t n)
{
  char *out = ccord_malloc(n + 1u);
  if (!out) return nullptr;
  for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)s[i]);
  out[n] = '\0';
  return out;
}

static char *dup_upper(const char *s)
{
  size_t n = strlen(s);
  char *out = ccord_malloc(n + 1u);
  if (!out) return nullptr;
  for (size_t i = 0; i < n; i++) out[i] = (char)toupper((unsigned char)s[i]);
  out[n] = '\0';
  return out;
}

static bool ascii_ieq(const char *a, const char *b)
{
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++; b++;
  }
  return *a == '\0' && *b == '\0';
}

static void camel_to_kebab(const char *in, char *out, size_t cap)
{
  if (in[0] == '-' && in[1] == '-') {
    snprintf(out, cap, "%s", in);
    return;
  }
  if (strcmp(in, "cssFloat") == 0 || strcmp(in, "styleFloat") == 0) {
    snprintf(out, cap, "float");
    return;
  }
  size_t w = 0;
  for (size_t i = 0; in[i] && w + 2u < cap; i++) {
    unsigned char c = (unsigned char)in[i];
    if (isupper(c)) {
      if (w > 0) out[w++] = '-';
      out[w++] = (char)tolower(c);
    } else {
      out[w++] = (char)c;
    }
  }
  out[w] = '\0';
}

// ---- Node allocation & tree ops ----

web_node *web_node_create(web_frame *f, web_node_type type, const char *tag)
{
  if (!f) return nullptr;
  web_node *n = ccord_calloc(1, sizeof *n);
  if (!n) return nullptr;
  n->id = ++f->next_node_id;
  n->frame = f;
  n->type = type;
  n->js_obj = JS_UNDEFINED;
  if (tag) {
    n->tag = (type == WEB_NODE_ELEMENT) ? dup_lower(tag, strlen(tag)) : ccord_strdup(tag);
  }
  if (f->all_nodes_len == f->all_nodes_cap) {
    size_t nc = f->all_nodes_cap ? f->all_nodes_cap * 2u : 64u;
    web_node **na = ccord_realloc(f->all_nodes, nc * sizeof *na);
    if (!na) { ccord_free(n->tag); ccord_free(n); return nullptr; }
    f->all_nodes = na;
    f->all_nodes_cap = nc;
  }
  f->all_nodes[f->all_nodes_len++] = n;
  return n;
}

static void free_listeners(JSContext *ctx, web_listener *l)
{
  while (l) {
    web_listener *next = l->next;
    ccord_free(l->type);
    if (ctx) JS_FreeValue(ctx, l->fn);
    ccord_free(l);
    l = next;
  }
}

void web_node_free_all_in_frame(web_frame *f)
{
  if (!f) return;
  free_listeners(f->ctx, f->win_listeners);
  f->win_listeners = nullptr;
  for (size_t i = 0; i < f->all_nodes_len; i++) {
    web_node *n = f->all_nodes[i];
    if (!n) continue;
    free_listeners(f->ctx, n->listeners);
    if (n->has_js_obj && f->ctx) {
      JS_SetOpaque(n->js_obj, nullptr);
      JS_FreeValue(f->ctx, n->js_obj);
      n->has_js_obj = false;
    }
    if (n->canvas) web_canvas_destroy(n->canvas);
    if (n->img) {
      ccord_free(n->img->rgba);
      ccord_free(n->img->svg_xml);
      ccord_free(n->img);
    }
    for (size_t j = 0; j < n->nattrs; j++) {
      ccord_free(n->attrs[j].name);
      ccord_free(n->attrs[j].value);
    }
    ccord_free(n->attrs);
    for (size_t j = 0; j < n->ninline; j++) {
      ccord_free(n->inline_props[j].name);
      ccord_free(n->inline_props[j].value);
    }
    ccord_free(n->inline_props);
    for (size_t j = 0; j < n->nvars; j++) {
      ccord_free(n->css_vars[j].name);
      ccord_free(n->css_vars[j].value);
    }
    ccord_free(n->css_vars);
    ccord_free(n->tag);
    ccord_free(n->ns);
    ccord_free(n->text);
    ccord_free(n);
  }
  ccord_free(f->all_nodes);
  f->all_nodes = nullptr;
  f->all_nodes_len = f->all_nodes_cap = 0;
  f->doc = f->head = f->body = nullptr;
}

void web_node_remove(web_node *child)
{
  if (!child || !child->parent) return;
  web_node *p = child->parent;
  if (child->prev_sib) child->prev_sib->next_sib = child->next_sib;
  else p->first_child = child->next_sib;
  if (child->next_sib) child->next_sib->prev_sib = child->prev_sib;
  else p->last_child = child->prev_sib;
  child->parent = child->prev_sib = child->next_sib = nullptr;
  if (child->frame) {
    child->frame->style_dirty = true;
    child->frame->layout_dirty = true;
    web_mark_dirty(child->frame->view);
  }
}

static bool is_ancestor_of(const web_node *anc, const web_node *n)
{
  for (const web_node *cur = n; cur; cur = cur->parent) {
    if (cur == anc) return true;
  }
  return false;
}

void web_node_append(web_node *parent, web_node *child)
{
  if (!parent || !child || parent == child || is_ancestor_of(child, parent)) return;
  if (child->type == WEB_NODE_FRAGMENT) {
    web_node *c = child->first_child;
    while (c) {
      web_node *next = c->next_sib;
      web_node_append(parent, c);
      c = next;
    }
    return;
  }
  web_node_remove(child);
  child->parent = parent;
  child->prev_sib = parent->last_child;
  child->next_sib = nullptr;
  if (parent->last_child) parent->last_child->next_sib = child;
  else parent->first_child = child;
  parent->last_child = child;
  if (parent->frame) {
    parent->frame->style_dirty = true;
    parent->frame->layout_dirty = true;
    web_mark_dirty(parent->frame->view);
  }
}

void web_node_insert_before(web_node *parent, web_node *child, web_node *ref)
{
  if (!parent || !child || parent == child || is_ancestor_of(child, parent)) return;
  if (!ref || ref->parent != parent) {
    web_node_append(parent, child);
    return;
  }
  if (child == ref) return;
  if (child->type == WEB_NODE_FRAGMENT) {
    web_node *c = child->first_child;
    while (c) {
      web_node *next = c->next_sib;
      web_node_insert_before(parent, c, ref);
      c = next;
    }
    return;
  }
  web_node_remove(child);
  child->parent = parent;
  child->next_sib = ref;
  child->prev_sib = ref->prev_sib;
  if (ref->prev_sib) ref->prev_sib->next_sib = child;
  else parent->first_child = child;
  ref->prev_sib = child;
  if (parent->frame) {
    parent->frame->style_dirty = true;
    parent->frame->layout_dirty = true;
    web_mark_dirty(parent->frame->view);
  }
}

static bool node_is_connected(const web_node *n)
{
  while (n) {
    if (n->type == WEB_NODE_DOCUMENT) return true;
    n = n->parent;
  }
  return false;
}

// ---- Attributes & inline style ----

const char *web_node_get_attr(const web_node *n, const char *name)
{
  if (!n || !name) return nullptr;
  for (size_t i = 0; i < n->nattrs; i++) {
    if (ascii_ieq(n->attrs[i].name, name)) return n->attrs[i].value;
  }
  return nullptr;
}

static void activate_iframe(web_node *n);
static void activate_image(web_node *n);
static void activate_script(web_node *n);
static void activate_style_or_link(web_node *n);

void web_node_set_attr(web_node *n, const char *name, const char *val)
{
  if (!n || !name) return;
  if (!val) val = "";
  for (size_t i = 0; i < n->nattrs; i++) {
    if (ascii_ieq(n->attrs[i].name, name)) {
      char *nv = ccord_strdup(val);
      if (nv) { ccord_free(n->attrs[i].value); n->attrs[i].value = nv; }
      goto updated;
    }
  }
  if (n->nattrs == n->attr_cap) {
    size_t nc = n->attr_cap ? n->attr_cap * 2u : 8u;
    web_attr *na = ccord_realloc(n->attrs, nc * sizeof *na);
    if (!na) return;
    n->attrs = na;
    n->attr_cap = nc;
  }
  n->attrs[n->nattrs].name = dup_lower(name, strlen(name));
  n->attrs[n->nattrs].value = ccord_strdup(val);
  n->nattrs++;

updated:
  if (ascii_ieq(name, "style")) {
    web_css_parse_inline(n, val);
  } else if (n->tag && strcmp(n->tag, "canvas") == 0 && (ascii_ieq(name, "width") || ascii_ieq(name, "height"))) {
    if (n->canvas) {
      const char *ws = web_node_get_attr(n, "width");
      const char *hs = web_node_get_attr(n, "height");
      web_canvas_resize(n->canvas, ws ? atoi(ws) : 300, hs ? atoi(hs) : 150);
    }
  } else if (n->tag && strcmp(n->tag, "iframe") == 0 && ascii_ieq(name, "src") && node_is_connected(n)) {
    activate_iframe(n);
  } else if (n->tag && strcmp(n->tag, "script") == 0 && ascii_ieq(name, "src") && node_is_connected(n)) {
    activate_script(n);
  } else if (n->tag && strcmp(n->tag, "link") == 0 && (ascii_ieq(name, "href") || ascii_ieq(name, "rel")) && node_is_connected(n)) {
    activate_style_or_link(n);
  } else if (n->tag && strcmp(n->tag, "img") == 0 && ascii_ieq(name, "src")) {
    activate_image(n);
  }
  if (n->frame) {
    n->frame->style_dirty = true;
    n->frame->layout_dirty = true;
    web_mark_dirty(n->frame->view);
  }
}

void web_node_remove_attr(web_node *n, const char *name)
{
  if (!n || !name) return;
  for (size_t i = 0; i < n->nattrs; i++) {
    if (ascii_ieq(n->attrs[i].name, name)) {
      ccord_free(n->attrs[i].name);
      ccord_free(n->attrs[i].value);
      for (size_t j = i + 1u; j < n->nattrs; j++) n->attrs[j - 1u] = n->attrs[j];
      n->nattrs--;
      if (ascii_ieq(name, "style")) web_css_parse_inline(n, "");
      if (n->frame) { n->frame->style_dirty = true; n->frame->layout_dirty = true; web_mark_dirty(n->frame->view); }
      return;
    }
  }
}

void web_node_set_inline_style(web_node *n, const char *prop, const char *val)
{
  if (!n || !prop || !prop[0]) return;
  char kebab[96];
  camel_to_kebab(prop, kebab, sizeof kebab);
  if (!val || !val[0]) {
    for (size_t i = 0; i < n->ninline; i++) {
      if (ascii_ieq(n->inline_props[i].name, kebab)) {
        ccord_free(n->inline_props[i].name);
        ccord_free(n->inline_props[i].value);
        for (size_t j = i + 1u; j < n->ninline; j++) n->inline_props[j - 1u] = n->inline_props[j];
        n->ninline--;
        break;
      }
    }
  } else {
    for (size_t i = 0; i < n->ninline; i++) {
      if (ascii_ieq(n->inline_props[i].name, kebab)) {
        char *nv = ccord_strdup(val);
        if (nv) { ccord_free(n->inline_props[i].value); n->inline_props[i].value = nv; }
        goto done;
      }
    }
    if (n->ninline == n->inline_cap) {
      size_t nc = n->inline_cap ? n->inline_cap * 2u : 8u;
      web_style_prop *np = ccord_realloc(n->inline_props, nc * sizeof *np);
      if (!np) return;
      n->inline_props = np;
      n->inline_cap = nc;
    }
    n->inline_props[n->ninline].name = ccord_strdup(kebab);
    n->inline_props[n->ninline].value = ccord_strdup(val);
    n->ninline++;
  }
done:
  if (n->frame) {
    n->frame->style_dirty = true;
    n->frame->layout_dirty = true;
    web_mark_dirty(n->frame->view);
  }
}

const char *web_node_get_inline_style(const web_node *n, const char *prop)
{
  if (!n || !prop) return "";
  char kebab[96];
  camel_to_kebab(prop, kebab, sizeof kebab);
  for (size_t i = 0; i < n->ninline; i++) {
    if (ascii_ieq(n->inline_props[i].name, kebab)) return n->inline_props[i].value;
  }
  return "";
}

// ---- Element activation (<script>, <style>, <link>, <iframe>, <img>) ----

static void collect_text(const web_node *n, ccord_buf *b)
{
  if (!n) return;
  if (n->type == WEB_NODE_TEXT && n->text) {
    (void)ccord_buf_append_str(b, n->text);
    return;
  }
  for (const web_node *c = n->first_child; c; c = c->next_sib) collect_text(c, b);
}

static void activate_image(web_node *n)
{
  if (!n || !n->frame) return;
  const char *src = web_node_get_attr(n, "src");
  if (!src || !src[0]) return;
  if (!n->img) {
    n->img = ccord_calloc(1, sizeof *n->img);
    if (!n->img) return;
    snprintf(n->img->tex_key, sizeof n->img->tex_key, "web_img_%u_%u", n->frame->id, n->id);
  }
  web_net_fetch_image(n->frame, n, n->img, src);
}

static void activate_iframe(web_node *n)
{
  if (!n || !n->frame || !n->frame->view) return;
  ccord_web_view *v = n->frame->view;
  const char *src = web_node_get_attr(n, "src");
  if (!src || !src[0] || strcmp(src, "about:blank") == 0) {
    if (!n->child_frame) {
      float w = n->box.w > 10.0f ? n->box.w : 320.0f;
      float h = n->box.h > 10.0f ? n->box.h : 100.0f;
      n->child_frame = web_frame_create(v, n->frame, n, "about:blank", w, h, false);
      if (n->child_frame) {
        (void)web_dom_parse_document(n->child_frame, "<!doctype html><html><head></head><body></body></html>", 54);
      }
    }
    return;
  }
  web_url *u = web_url_alloc_parse(src, &n->frame->url);
  if (!u) return;
  if (n->child_frame && strcmp(n->child_frame->url.href, u->href) == 0) {
    ccord_free(u);
    return;
  }

  float w = n->box.w > 10.0f ? n->box.w : (strstr(u->href, "frame=checkbox") ? 302.0f : (n->frame->viewport_w > 0 ? n->frame->viewport_w : 400.0f));
  float h = n->box.h > 10.0f ? n->box.h : (strstr(u->href, "frame=checkbox") ? 76.0f : 500.0f);
  const char *wa = web_node_get_attr(n, "width"), *ha = web_node_get_attr(n, "height");
  const char *sw = web_node_get_inline_style(n, "width"), *sh = web_node_get_inline_style(n, "height");
  if ((!wa || !wa[0]) && sw && sw[0]) wa = sw;
  if ((!ha || !ha[0]) && sh && sh[0]) ha = sh;
  if (wa && wa[0]) {
    if (strchr(wa, '%')) {
      float p = (float)strtod(wa, nullptr);
      if (p > 0.0f) w = n->frame->viewport_w * (p * 0.01f);
    } else if (atoi(wa) > 0) {
      w = (float)atoi(wa);
    }
  }
  if (ha && ha[0]) {
    if (strchr(ha, '%')) {
      float p = (float)strtod(ha, nullptr);
      if (p > 0.0f) h = n->frame->viewport_h * (p * 0.01f);
    } else if (atoi(ha) > 0) {
      h = (float)atoi(ha);
    }
  }

  if (n->child_frame) {
    web_frame_destroy(n->child_frame);
    n->child_frame = nullptr;
  }
  n->child_frame = web_frame_create(v, n->frame, n, u->href, w, h, false);
  if (!n->child_frame) { ccord_free(u); return; }

  web_diag_append(v, "info", "iframe loading: %s", u->href);
  web_fetch *ft = web_net_start(v, n->child_frame, WEB_FETCH_IFRAME, u->href, "GET", nullptr, 0, nullptr, 0);
  ccord_free(u);
  if (ft) {
    ft->node = n;
  } else {
    web_dom_dispatch_simple(n->frame, n, "error", false);
  }
}

static void activate_style_or_link(web_node *n)
{
  if (!n || !n->frame || !n->tag) return;
  if (strcmp(n->tag, "style") == 0) {
    ccord_buf css;
    ccord_buf_init(&css, 2u * 1024u * 1024u);
    collect_text(n, &css);
    if (css.len > 0) {
      web_css_parse_sheet(&n->frame->sheet, css.data, css.len);
      n->frame->style_dirty = true;
      n->frame->layout_dirty = true;
      web_mark_dirty(n->frame->view);
    }
    ccord_buf_free(&css);
  } else if (strcmp(n->tag, "link") == 0 && !n->script_executed) {
    const char *rel = web_node_get_attr(n, "rel");
    const char *href = web_node_get_attr(n, "href");
    if (rel && ascii_ieq(rel, "stylesheet") && href && href[0]) {
      n->script_executed = true;
      web_fetch *ft = web_net_start(n->frame->view, n->frame, WEB_FETCH_STYLE, href, "GET", nullptr, 0, nullptr, 0);
      if (ft) ft->node = n;
    }
  }
}

void web_dom_exec_script_code(web_frame *f, web_node *script_node, const char *code, size_t len, const char *filename)
{
  if (!f || !f->ctx || !code || len == 0) return;
  if (f->view && f->view->rt) JS_UpdateStackTop(f->view->rt);
  web_node *prev = f->current_script;
  f->current_script = script_node;
  JSValue res = JS_Eval(f->ctx, code, len, filename ? filename : "<inline>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(res)) {
    web_js_log_exception(f, filename ? filename : "<script>");
  }
  JS_FreeValue(f->ctx, res);
  f->current_script = prev;
  web_drain_jobs(f->view);
}

static void activate_script(web_node *n)
{
  if (!n || !n->frame || n->script_executed) return;
  const char *type = web_node_get_attr(n, "type");
  if (type && type[0] && !ascii_ieq(type, "text/javascript") && !ascii_ieq(type, "application/javascript") &&
      !ascii_ieq(type, "text/ecmascript") && !ascii_ieq(type, "module")) {
    return;
  }
  const char *src = web_node_get_attr(n, "src");
  if (src && src[0]) {
    n->script_executed = true;
    web_diag_append(n->frame->view, "info", "script fetch: %s", src);
    web_fetch *ft = web_net_start(n->frame->view, n->frame, WEB_FETCH_SCRIPT, src, "GET", nullptr, 0, nullptr, 0);
    if (ft) {
      ft->node = n;
      n->frame->pending_scripts++;
    } else {
      web_dom_dispatch_simple(n->frame, n, "error", false);
    }
  } else {
    ccord_buf code;
    ccord_buf_init(&code, 8u * 1024u * 1024u);
    collect_text(n, &code);
    if (code.len > 0 && code.data && !code.failed) {
      n->script_executed = true;
      web_dom_exec_script_code(n->frame, n, code.data, code.len, n->frame->url.href);
    }
    ccord_buf_free(&code);
  }
}

static void activate_subtree_on_insert(web_node *n)
{
  if (!n || !n->frame || !n->frame->view || !node_is_connected(n)) return;
  ccord_web_view *v = n->frame->view;
  uint32_t fid = n->frame->id;
  if (n->type == WEB_NODE_ELEMENT && n->tag) {
    if (strcmp(n->tag, "script") == 0) activate_script(n);
    else if (strcmp(n->tag, "style") == 0 || strcmp(n->tag, "link") == 0) activate_style_or_link(n);
    else if (strcmp(n->tag, "iframe") == 0) activate_iframe(n);
    else if (strcmp(n->tag, "img") == 0) activate_image(n);
  }
  if (!web_frame_alive(v, fid)) return;
  for (web_node *c = n->first_child; c; ) {
    web_node *next = c->next_sib;
    activate_subtree_on_insert(c);
    if (!web_frame_alive(v, fid)) return;
    c = next;
  }
}

void web_dom_on_fetch_done(web_fetch *ft)
{
  if (!ft || !ft->frame || !ft->view) return;
  ccord_web_view *v = ft->view;
  web_frame *f = ft->frame;
  uint32_t fid = f->id;
  if (ft->kind == WEB_FETCH_SCRIPT) {
    if (f->pending_scripts > 0) f->pending_scripts--;
    if (!ft->failed && ft->http_status >= 200 && ft->http_status < 300 && ft->resp_body.data) {
      web_dom_exec_script_code(f, ft->node, ft->resp_body.data, ft->resp_body.len, ft->final_url);
      if (!web_frame_alive(v, fid)) return;
      if (ft->node) web_dom_dispatch_simple(f, ft->node, "load", false);
    } else {
      web_diag_append(v, "error", "script load failed (%ld): %s", ft->http_status, ft->url);
      if (ft->node) web_dom_dispatch_simple(f, ft->node, "error", false);
    }
    if (!web_frame_alive(v, fid)) return;
    if (f->pending_scripts == 0 && !f->loaded) {
      f->loaded = true;
      if (f->doc) web_dom_dispatch_simple(f, f->doc, "readystatechange", false);
      if (!web_frame_alive(v, fid)) return;
      JSValue ev = web_js_make_event(f->ctx, "load", false, false);
      web_dom_dispatch_win_event(f, ev);
      if (web_frame_alive(v, fid)) JS_FreeValue(f->ctx, ev);
    }
  } else if (ft->kind == WEB_FETCH_STYLE) {
    if (!ft->failed && ft->http_status >= 200 && ft->http_status < 300 && ft->resp_body.data) {
      web_css_parse_sheet(&f->sheet, ft->resp_body.data, ft->resp_body.len);
      f->style_dirty = true;
      f->layout_dirty = true;
      web_mark_dirty(ft->view);
      if (ft->node) web_dom_dispatch_simple(f, ft->node, "load", false);
    } else if (ft->node) {
      web_dom_dispatch_simple(f, ft->node, "error", false);
    }
  } else if (ft->kind == WEB_FETCH_IFRAME) {
    uint32_t parent_fid = (ft->node && ft->node->frame) ? ft->node->frame->id : 0u;
    uint32_t child_fid = f->id;
    web_node *host_node = ft->node;
    if (!ft->failed && ft->http_status >= 200 && ft->http_status < 400 && ft->resp_body.data) {
      web_diag_append(v, "info", "iframe[f%u] parsing (%zu B)", child_fid, ft->resp_body.len);
      (void)web_url_parse(ft->final_url, nullptr, &f->url);
      web_js_sync_location(f);
      if (web_dom_parse_document(f, ft->resp_body.data, ft->resp_body.len)) {
        web_diag_append(v, "info", "iframe[f%u] parsed (%zu nodes), running scripts", child_fid, f->all_nodes_len);
        web_dom_run_scripts(f);
        web_diag_append(v, "info", "iframe[f%u] scripts done", child_fid);
      }
      if (parent_fid && web_frame_alive(v, parent_fid) && host_node && host_node->frame) {
        web_dom_dispatch_simple(host_node->frame, host_node, "load", false);
      }
    } else {
      web_diag_append(v, "error", "iframe load failed (%ld): %.160s", ft->http_status, ft->url);
      if (parent_fid && web_frame_alive(v, parent_fid) && host_node && host_node->frame) {
        web_dom_dispatch_simple(host_node->frame, host_node, "error", false);
      }
    }
  }
}

// ---- Lexbor HTML5 -> web_node tree ----

static void import_lexbor_children(web_frame *f, lxb_dom_node_t *lxb_parent, web_node *parent)
{
  for (lxb_dom_node_t *ln = lxb_parent->first_child; ln; ln = ln->next) {
    if (ln->type == LXB_DOM_NODE_TYPE_ELEMENT) {
      size_t tlen = 0;
      const lxb_char_t *tname = lxb_dom_node_name(ln, &tlen);
      char tagbuf[64];
      size_t cl = tname ? (tlen < sizeof tagbuf - 1u ? tlen : sizeof tagbuf - 1u) : 0u;
      for (size_t i = 0; i < cl; i++) tagbuf[i] = (char)tolower(tname[i]);
      tagbuf[cl] = '\0';
      web_node *el = web_node_create(f, WEB_NODE_ELEMENT, tagbuf);
      if (!el) continue;
      lxb_dom_element_t *lel = lxb_dom_interface_element(ln);
      for (lxb_dom_attr_t *a = lxb_dom_element_first_attribute(lel); a; a = lxb_dom_element_next_attribute(a)) {
        size_t nlen = 0, vlen = 0;
        const lxb_char_t *an = lxb_dom_attr_local_name(a, &nlen);
        const lxb_char_t *av = lxb_dom_attr_value(a, &vlen);
        if (!an || nlen == 0) continue;
        char *ns = dup_lower((const char *)an, nlen);
        char *vs = ccord_malloc(vlen + 1u);
        if (ns && vs) {
          if (av && vlen > 0) memcpy(vs, av, vlen);
          vs[vlen] = '\0';
          web_node_set_attr(el, ns, vs);
        }
        ccord_free(ns);
        ccord_free(vs);
      }
      web_node_append(parent, el);
      if (strcmp(tagbuf, "head") == 0 && !f->head) f->head = el;
      if (strcmp(tagbuf, "body") == 0 && !f->body) f->body = el;
      import_lexbor_children(f, ln, el);
    } else if (ln->type == LXB_DOM_NODE_TYPE_TEXT) {
      lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(ln);
      web_node *tn = web_node_create(f, WEB_NODE_TEXT, "#text");
      if (tn) {
        size_t n = cd->data.length;
        tn->text = ccord_malloc(n + 1u);
        if (tn->text) {
          if (cd->data.data && n > 0) memcpy(tn->text, cd->data.data, n);
          tn->text[n] = '\0';
        }
        web_node_append(parent, tn);
      }
    } else if (ln->type == LXB_DOM_NODE_TYPE_COMMENT) {
      lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(ln);
      web_node *cn = web_node_create(f, WEB_NODE_COMMENT, "#comment");
      if (cn) {
        size_t n = cd->data.length;
        cn->text = ccord_malloc(n + 1u);
        if (cn->text) {
          if (cd->data.data && n > 0) memcpy(cn->text, cd->data.data, n);
          cn->text[n] = '\0';
        }
        web_node_append(parent, cn);
      }
    }
  }
}

bool web_dom_parse_document(web_frame *f, const char *html, size_t len)
{
  if (!f || !html) return false;
  lxb_html_document_t *ldoc = lxb_html_document_create();
  if (!ldoc) return false;
  lxb_status_t st = lxb_html_document_parse(ldoc, (const lxb_char_t *)html, len);
  if (st != LXB_STATUS_OK) {
    lxb_html_document_destroy(ldoc);
    return false;
  }
  if (!f->doc) f->doc = web_node_create(f, WEB_NODE_DOCUMENT, "#document");
  // Clear existing document children
  while (f->doc->first_child) web_node_remove(f->doc->first_child);
  f->head = nullptr;
  f->body = nullptr;
  web_css_sheet_free(&f->sheet);

  import_lexbor_children(f, lxb_dom_interface_node(ldoc), f->doc);
  lxb_html_document_destroy(ldoc);

  // Ensure html/head/body exist even for bare snippets
  web_node *html_el = web_dom_query_one(f->doc, "html");
  if (!html_el) {
    html_el = web_node_create(f, WEB_NODE_ELEMENT, "html");
    web_node_append(f->doc, html_el);
  }
  if (!f->head) {
    f->head = web_node_create(f, WEB_NODE_ELEMENT, "head");
    web_node_insert_before(html_el, f->head, html_el->first_child);
  }
  if (!f->body) {
    f->body = web_node_create(f, WEB_NODE_ELEMENT, "body");
    web_node_append(html_el, f->body);
  }
  f->style_dirty = true;
  f->layout_dirty = true;
  web_mark_dirty(f->view);
  return true;
}

bool web_dom_parse_fragment_ex(web_frame *f, web_node *parent, const char *html, size_t len, bool clear_existing)
{
  if (!f || !parent) return false;
  if (clear_existing) {
    while (parent->first_child) web_node_remove(parent->first_child);
  }
  if (!html || len == 0) return true;

  web_node *prev_last = parent->last_child;
  lxb_html_document_t *ldoc = lxb_html_document_create();
  if (!ldoc) return false;
  (void)lxb_html_document_parse(ldoc, (const lxb_char_t *)"<!doctype html><html><body></body></html>", 41);
  lxb_dom_element_t *ctx_el = lxb_dom_interface_element(lxb_html_document_body_element(ldoc));
  lxb_dom_node_t *frag = lxb_html_document_parse_fragment(ldoc, ctx_el, (const lxb_char_t *)html, len);
  if (frag) {
    import_lexbor_children(f, frag, parent);
  }
  lxb_html_document_destroy(ldoc);
  if (node_is_connected(parent)) {
    web_node *start = prev_last ? prev_last->next_sib : parent->first_child;
    for (web_node *c = start; c; c = c->next_sib) activate_subtree_on_insert(c);
  }
  f->style_dirty = true;
  f->layout_dirty = true;
  web_mark_dirty(f->view);
  return frag != nullptr;
}

bool web_dom_parse_fragment(web_frame *f, web_node *parent, const char *html, size_t len)
{
  return web_dom_parse_fragment_ex(f, parent, html, len, true);
}

static void collect_initial_elements(web_node *n, web_node **scripts, size_t *nscripts, size_t cap)
{
  if (!n) return;
  if (n->type == WEB_NODE_ELEMENT && n->tag) {
    if (strcmp(n->tag, "style") == 0 || strcmp(n->tag, "link") == 0) activate_style_or_link(n);
    else if (strcmp(n->tag, "img") == 0) activate_image(n);
    else if (strcmp(n->tag, "iframe") == 0) activate_iframe(n);
    else if (strcmp(n->tag, "script") == 0 && *nscripts < cap) scripts[(*nscripts)++] = n;
  }
  for (web_node *c = n->first_child; c; c = c->next_sib) collect_initial_elements(c, scripts, nscripts, cap);
}

void web_dom_run_scripts(web_frame *f)
{
  if (!f || !f->doc) return;
  web_node *scripts[128];
  size_t nscripts = 0;
  collect_initial_elements(f->doc, scripts, &nscripts, 128u);
  for (size_t i = 0; i < nscripts; i++) activate_script(scripts[i]);
  f->dom_content_loaded = true;
  web_dom_dispatch_simple(f, f->doc, "readystatechange", false);
  JSValue dcl = web_js_make_event(f->ctx, "DOMContentLoaded", true, false);
  web_dom_dispatch_event(f, f->doc, dcl);
  JS_FreeValue(f->ctx, dcl);
  if (f->pending_scripts == 0 && !f->loaded) {
    f->loaded = true;
    web_dom_dispatch_simple(f, f->doc, "readystatechange", false);
    JSValue lev = web_js_make_event(f->ctx, "load", false, false);
    web_dom_dispatch_win_event(f, lev);
    JS_FreeValue(f->ctx, lev);
  }
}

// ---- CSS selector matcher ----

static bool class_list_contains(const char *cls_attr, const char *needle, size_t nlen)
{
  if (!cls_attr || nlen == 0) return false;
  const char *p = cls_attr;
  while (*p) {
    while (*p && isspace((unsigned char)*p)) p++;
    const char *s = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if ((size_t)(p - s) == nlen && memcmp(s, needle, nlen) == 0) return true;
  }
  return false;
}

static bool match_compound(const web_node *n, const char *sel, size_t len)
{
  if (!n || n->type != WEB_NODE_ELEMENT) return false;
  size_t i = 0;
  while (i < len) {
    if (sel[i] == '*') {
      i++;
    } else if (sel[i] == '#') {
      i++;
      size_t st = i;
      while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++;
      const char *id = web_node_get_attr(n, "id");
      if (!id || strlen(id) != i - st || memcmp(id, sel + st, i - st) != 0) return false;
    } else if (sel[i] == '.') {
      i++;
      size_t st = i;
      while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++;
      if (!class_list_contains(web_node_get_attr(n, "class"), sel + st, i - st)) return false;
    } else if (sel[i] == '[') {
      i++;
      size_t ns = i;
      while (i < len && sel[i] != ']' && sel[i] != '=' && sel[i] != '^' && sel[i] != '*' && sel[i] != '$' && sel[i] != '~') i++;
      char aname[64];
      size_t nl = i - ns < sizeof aname - 1u ? i - ns : sizeof aname - 1u;
      memcpy(aname, sel + ns, nl);
      aname[nl] = '\0';
      const char *aval = web_node_get_attr(n, aname);
      char op = 0;
      if (i < len && (sel[i] == '^' || sel[i] == '*' || sel[i] == '$' || sel[i] == '~')) { op = sel[i]; i++; }
      if (i < len && sel[i] == '=') {
        i++;
        if (i < len && (sel[i] == '"' || sel[i] == '\'')) i++;
        size_t vs = i;
        while (i < len && sel[i] != ']' && sel[i] != '"' && sel[i] != '\'') i++;
        size_t vl = i - vs;
        while (i < len && sel[i] != ']') i++;
        if (i < len && sel[i] == ']') i++;
        if (!aval) return false;
        size_t al = strlen(aval);
        if (op == 0 && (al != vl || memcmp(aval, sel + vs, vl) != 0)) return false;
        if (op == '^' && (al < vl || memcmp(aval, sel + vs, vl) != 0)) return false;
        if (op == '$' && (al < vl || memcmp(aval + (al - vl), sel + vs, vl) != 0)) return false;
        if (op == '~' && !class_list_contains(aval, sel + vs, vl)) return false;
        if (op == '*') {
          char tmp[128];
          size_t cl = vl < sizeof tmp - 1u ? vl : sizeof tmp - 1u;
          memcpy(tmp, sel + vs, cl); tmp[cl] = '\0';
          if (!strstr(aval, tmp)) return false;
        }
      } else {
        while (i < len && sel[i] != ']') i++;
        if (i < len && sel[i] == ']') i++;
        if (!aval) return false;
      }
    } else if (sel[i] == ':') {
      // Double-colon pseudo-elements (::before, ::after, ::placeholder, ::-webkit-*, ::selection) never match real elements
      if (i + 1u < len && sel[i + 1u] == ':') return false;
      i++;
      size_t ps = i;
      while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':' && sel[i] != '(') i++;
      size_t pl = i - ps;
      const char *arg_s = nullptr;
      size_t arg_l = 0;
      if (i < len && sel[i] == '(') {
        int dep = 1; i++;
        arg_s = sel + i;
        while (i < len && dep > 0) {
          if (sel[i] == '(') dep++;
          else if (sel[i] == ')') dep--;
          if (dep > 0) i++;
        }
        arg_l = (size_t)((sel + i) - arg_s);
        if (i < len && sel[i] == ')') i++;
      }
      if (pl == 5 && memcmp(sel + ps, "hover", 5) == 0) {
        if (!n->frame || n->frame->hovered_node != n) return false;
      } else if (pl == 5 && memcmp(sel + ps, "focus", 5) == 0) {
        if (!n->frame || n->frame->focused_node != n) return false;
      } else if (pl == 13 && memcmp(sel + ps, "focus-visible", 13) == 0) {
        if (!n->frame || n->frame->focused_node != n) return false;
      } else if (pl == 12 && memcmp(sel + ps, "focus-within", 12) == 0) {
        if (!n->frame || !n->frame->focused_node || !is_ancestor_of(n, n->frame->focused_node)) return false;
      } else if (pl == 6 && memcmp(sel + ps, "active", 6) == 0) {
        if (!n->frame || !n->frame->view || !n->frame->view->mouse_down || n->frame->hovered_node != n) return false;
      } else if (pl == 7 && memcmp(sel + ps, "checked", 7) == 0) {
        if (!n->checked) return false;
      } else if (pl == 8 && memcmp(sel + ps, "disabled", 8) == 0) {
        if (!web_node_get_attr(n, "disabled")) return false;
      } else if (pl == 7 && memcmp(sel + ps, "enabled", 7) == 0) {
        if (web_node_get_attr(n, "disabled")) return false;
      } else if (pl == 5 && memcmp(sel + ps, "empty", 5) == 0) {
        for (const web_node *ch = n->first_child; ch; ch = ch->next_sib) {
          if (ch->type == WEB_NODE_ELEMENT) return false;
          if (ch->type == WEB_NODE_TEXT && ch->text && ch->text[0]) return false;
        }
      } else if (pl == 4 && memcmp(sel + ps, "root", 4) == 0) {
        if (!n->tag || strcmp(n->tag, "html") != 0) return false;
      } else if (pl == 11 && memcmp(sel + ps, "first-child", 11) == 0) {
        for (const web_node *p = n->prev_sib; p; p = p->prev_sib) {
          if (p->type == WEB_NODE_ELEMENT) return false;
        }
      } else if (pl == 10 && memcmp(sel + ps, "last-child", 10) == 0) {
        for (const web_node *p = n->next_sib; p; p = p->next_sib) {
          if (p->type == WEB_NODE_ELEMENT) return false;
        }
      } else if (pl == 10 && memcmp(sel + ps, "only-child", 10) == 0) {
        for (const web_node *p = n->prev_sib; p; p = p->prev_sib) { if (p->type == WEB_NODE_ELEMENT) return false; }
        for (const web_node *p = n->next_sib; p; p = p->next_sib) { if (p->type == WEB_NODE_ELEMENT) return false; }
      } else if (pl == 3 && memcmp(sel + ps, "not", 3) == 0 && arg_s && arg_l > 0) {
        char sub[256];
        size_t sl = arg_l < sizeof sub - 1u ? arg_l : sizeof sub - 1u;
        memcpy(sub, arg_s, sl); sub[sl] = '\0';
        if (web_dom_matches_selector(n, sub)) return false;
      } else if (((pl == 2 && memcmp(sel + ps, "is", 2) == 0) || (pl == 5 && memcmp(sel + ps, "where", 5) == 0)) &&
                 arg_s && arg_l > 0) {
        char sub[256];
        size_t sl = arg_l < sizeof sub - 1u ? arg_l : sizeof sub - 1u;
        memcpy(sub, arg_s, sl); sub[sl] = '\0';
        if (!web_dom_matches_selector(n, sub)) return false;
      } else if (pl == 4 && memcmp(sel + ps, "link", 4) == 0) {
        if (!n->tag || strcmp(n->tag, "a") != 0) return false;
      } else if (pl == 8 && memcmp(sel + ps, "any-link", 8) == 0) {
        if (!n->tag || strcmp(n->tag, "a") != 0 || !web_node_get_attr(n, "href")) return false;
      } else if (pl == 3 && memcmp(sel + ps, "dir", 3) == 0 && arg_s && arg_l > 0) {
        if (arg_l == 3 && ccord_strncasecmp(arg_s, "rtl", 3) == 0) return false;
      } else {
        // Unrecognized pseudo-class / pseudo-element: never falsely match arbitrary elements
        return false;
      }
    } else {
      size_t ts = i;
      while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != '[' && sel[i] != ':') i++;
      if (!n->tag || strlen(n->tag) != i - ts) return false;
      for (size_t k = 0; k < i - ts; k++) {
        if (tolower((unsigned char)sel[ts + k]) != n->tag[k]) return false;
      }
    }
  }
  return true;
}

static bool match_single_selector(const web_node *n, const char *s, size_t len)
{
  // Tokenize right-to-left by whitespace / '>' combinators
  typedef struct part { const char *p; size_t len; char comb; } part;
  part parts[16];
  size_t np = 0;
  size_t i = 0;
  char next_comb = ' ';
  while (i < len && np < 16u) {
    while (i < len && isspace((unsigned char)s[i])) i++;
    if (i >= len) break;
    if (s[i] == '>' || s[i] == '+' || s[i] == '~') { next_comb = s[i++]; continue; }
    size_t st = i;
    int bracket = 0, paren = 0;
    while (i < len) {
      if (s[i] == '[') bracket++;
      else if (s[i] == ']' && bracket > 0) bracket--;
      else if (s[i] == '(') paren++;
      else if (s[i] == ')' && paren > 0) paren--;
      else if (bracket == 0 && paren == 0 && (isspace((unsigned char)s[i]) || s[i] == '>' || s[i] == '+' || s[i] == '~')) break;
      i++;
    }
    char comb = np == 0 ? 0 : next_comb;
    parts[np++] = (part){s + st, i - st, comb};
    next_comb = ' ';
  }
  if (np == 0) return false;
  if (!match_compound(n, parts[np - 1u].p, parts[np - 1u].len)) return false;

  const web_node *cur = n;
  for (size_t idx = np - 1u; idx > 0; idx--) {
    char comb = parts[idx].comb;
    const part *prev = &parts[idx - 1u];
    if (comb == '>') {
      cur = cur->parent;
      if (!cur || !match_compound(cur, prev->p, prev->len)) return false;
    } else if (comb == '+') {
      cur = cur->prev_sib;
      while (cur && cur->type != WEB_NODE_ELEMENT) cur = cur->prev_sib;
      if (!cur || !match_compound(cur, prev->p, prev->len)) return false;
    } else if (comb == '~') {
      cur = cur->prev_sib;
      bool found = false;
      while (cur) {
        if (cur->type == WEB_NODE_ELEMENT && match_compound(cur, prev->p, prev->len)) { found = true; break; }
        cur = cur->prev_sib;
      }
      if (!found) return false;
    } else {
      cur = cur->parent;
      bool found = false;
      while (cur) {
        if (match_compound(cur, prev->p, prev->len)) { found = true; break; }
        cur = cur->parent;
      }
      if (!found) return false;
    }
  }
  return true;
}

bool web_dom_matches_selector(const web_node *n, const char *selector)
{
  if (!n || !selector) return false;
  const char *p = selector;
  while (*p) {
    const char *st = p;
    int br = 0, pa = 0;
    while (*p) {
      if (*p == '[') br++; else if (*p == ']' && br > 0) br--;
      else if (*p == '(') pa++; else if (*p == ')' && pa > 0) pa--;
      else if (*p == ',' && br == 0 && pa == 0) break;
      p++;
    }
    if (match_single_selector(n, st, (size_t)(p - st))) return true;
    if (*p == ',') p++;
  }
  return false;
}

static void query_walk(web_node *n, const char *sel, web_node **out, size_t *count, size_t cap, bool first_only)
{
  for (web_node *c = n->first_child; c; c = c->next_sib) {
    if (c->type == WEB_NODE_ELEMENT && web_dom_matches_selector(c, sel)) {
      if (*count < cap) out[(*count)++] = c;
      if (first_only) return;
    }
    query_walk(c, sel, out, count, cap, first_only);
    if (first_only && *count > 0) return;
  }
}

web_node *web_dom_query_one(web_node *root, const char *selector)
{
  web_node *out = nullptr;
  size_t count = 0;
  if (root && selector) query_walk(root, selector, &out, &count, 1u, true);
  return count > 0 ? out : nullptr;
}

size_t web_dom_query_all(web_node *root, const char *selector, web_node **out, size_t cap)
{
  size_t count = 0;
  if (root && selector && out && cap > 0) query_walk(root, selector, out, &count, cap, false);
  return count;
}

// ---- Events & MutationObserver ----

static void add_listener(JSContext *ctx, web_listener **head, const char *type, JSValueConst fn, bool capture, bool once)
{
  if (!type || !type[0] || (!JS_IsFunction(ctx, fn) && !JS_IsObject(fn))) return;
  web_listener *l = ccord_calloc(1, sizeof *l);
  if (!l) return;
  l->type = ccord_strdup(type);
  l->fn = JS_DupValue(ctx, fn);
  l->capture = capture;
  l->once = once;
  // Append to tail to preserve registration order
  web_listener **pp = head;
  while (*pp) pp = &(*pp)->next;
  *pp = l;
}

static void remove_listener(JSContext *ctx, web_listener **head, const char *type, JSValueConst fn, bool capture)
{
  if (!type) return;
  web_listener **pp = head;
  while (*pp) {
    web_listener *l = *pp;
    if (strcmp(l->type, type) == 0 && l->capture == capture &&
        JS_VALUE_GET_PTR(l->fn) == JS_VALUE_GET_PTR(fn) && JS_VALUE_GET_TAG(l->fn) == JS_VALUE_GET_TAG(fn)) {
      *pp = l->next;
      ccord_free(l->type);
      JS_FreeValue(ctx, l->fn);
      ccord_free(l);
      return;
    }
    pp = &(*pp)->next;
  }
}

static void invoke_listeners(web_frame *f, web_listener **head, JSValueConst this_obj, JSValueConst ev_obj,
                             const char *type, bool capture_phase, bool *stop_prop, bool *stop_imm)
{
  JSContext *ctx = f->ctx;
  // Snapshot matching listeners so handlers mutating the list are safe
  JSValue fns[64];
  size_t nfns = 0;
  web_listener **pp = head;
  while (*pp && nfns < 64u) {
    web_listener *l = *pp;
    if (strcmp(l->type, type) == 0 && l->capture == capture_phase) {
      fns[nfns++] = JS_DupValue(ctx, l->fn);
      if (l->once) {
        *pp = l->next;
        ccord_free(l->type);
        JS_FreeValue(ctx, l->fn);
        ccord_free(l);
        continue;
      }
    }
    pp = &(*pp)->next;
  }
  for (size_t i = 0; i < nfns; i++) {
    if (!*stop_imm) {
      JSValue args[1] = {ev_obj};
      JSValue r = JS_UNDEFINED;
      if (JS_IsFunction(ctx, fns[i])) {
        r = JS_Call(ctx, fns[i], this_obj, 1, args);
      } else if (JS_IsObject(fns[i])) {
        JSValue he = JS_GetPropertyStr(ctx, fns[i], "handleEvent");
        if (JS_IsFunction(ctx, he)) r = JS_Call(ctx, he, fns[i], 1, args);
        JS_FreeValue(ctx, he);
      }
      if (JS_IsException(r)) web_js_log_exception(f, "event listener");
      JS_FreeValue(ctx, r);
      JSValue sp = JS_GetPropertyStr(ctx, ev_obj, "_stopProp");
      JSValue si = JS_GetPropertyStr(ctx, ev_obj, "_stopImm");
      if (JS_ToBool(ctx, sp) > 0) *stop_prop = true;
      if (JS_ToBool(ctx, si) > 0) { *stop_prop = true; *stop_imm = true; }
      JS_FreeValue(ctx, sp);
      JS_FreeValue(ctx, si);
    }
    JS_FreeValue(ctx, fns[i]);
  }
}

static void invoke_on_prop(web_frame *f, JSValueConst this_obj, JSValueConst ev_obj, const char *type)
{
  char prop[64];
  snprintf(prop, sizeof prop, "on%s", type);
  JSValue fn = JS_GetPropertyStr(f->ctx, this_obj, prop);
  if (JS_IsFunction(f->ctx, fn)) {
    JSValue args[1] = {ev_obj};
    JSValue r = JS_Call(f->ctx, fn, this_obj, 1, args);
    if (JS_IsException(r)) web_js_log_exception(f, prop);
    JS_FreeValue(f->ctx, r);
  }
  JS_FreeValue(f->ctx, fn);
}

void web_dom_dispatch_win_event(web_frame *f, JSValue ev_obj)
{
  if (!f || !f->ctx || !f->view) return;
  ccord_web_view *v = f->view;
  uint32_t fid = f->id;
  JSValue tv = JS_GetPropertyStr(f->ctx, ev_obj, "type");
  const char *type = JS_ToCString(f->ctx, tv);
  JS_FreeValue(f->ctx, tv);
  if (!type) { JSValue ex = JS_GetException(f->ctx); JS_FreeValue(f->ctx, ex); return; }
  JS_SetPropertyStr(f->ctx, ev_obj, "target", JS_DupValue(f->ctx, f->win_obj));
  JS_SetPropertyStr(f->ctx, ev_obj, "currentTarget", JS_DupValue(f->ctx, f->win_obj));
  bool sp = false, si = false;
  invoke_listeners(f, &f->win_listeners, f->win_obj, ev_obj, type, true, &sp, &si);
  if (!web_frame_alive(v, fid)) return;
  if (!si) invoke_listeners(f, &f->win_listeners, f->win_obj, ev_obj, type, false, &sp, &si);
  if (!web_frame_alive(v, fid)) return;
  if (!si) invoke_on_prop(f, f->win_obj, ev_obj, type);
  if (web_frame_alive(v, fid)) JS_FreeCString(f->ctx, type);
  web_drain_jobs(v);
}

void web_dom_dispatch_event(web_frame *f, web_node *target, JSValue ev_obj)
{
  if (!f || !f->ctx || !f->view || !target) return;
  ccord_web_view *v = f->view;
  uint32_t fid = f->id;
  JSValue tv = JS_GetPropertyStr(f->ctx, ev_obj, "type");
  const char *type = JS_ToCString(f->ctx, tv);
  JS_FreeValue(f->ctx, tv);
  if (!type) { JSValue ex = JS_GetException(f->ctx); JS_FreeValue(f->ctx, ex); return; }
  JSValue bv = JS_GetPropertyStr(f->ctx, ev_obj, "bubbles");
  bool bubbles = JS_ToBool(f->ctx, bv) > 0;
  JS_FreeValue(f->ctx, bv);

  web_node *path[64];
  size_t plen = 0;
  for (web_node *cur = target; cur && plen < 64u; cur = cur->parent) path[plen++] = cur;

  JSValue target_js = web_node_wrap(f, target);
  JS_SetPropertyStr(f->ctx, ev_obj, "target", JS_DupValue(f->ctx, target_js));

  bool sp = false, si = false;
  // Capture phase (window -> document -> ... -> target)
  invoke_listeners(f, &f->win_listeners, f->win_obj, ev_obj, type, true, &sp, &si);
  if (!web_frame_alive(v, fid)) return;
  for (size_t i = plen; i > 0 && !sp; i--) {
    web_node *n = path[i - 1u];
    JSValue nw = web_node_wrap(f, n);
    JS_SetPropertyStr(f->ctx, ev_obj, "currentTarget", JS_DupValue(f->ctx, nw));
    invoke_listeners(f, &n->listeners, nw, ev_obj, type, true, &sp, &si);
    if (!web_frame_alive(v, fid)) return;
    JS_FreeValue(f->ctx, nw);
  }
  // Target + Bubble phase
  for (size_t i = 0; i < plen && !sp; i++) {
    if (i > 0 && !bubbles) break;
    web_node *n = path[i];
    JSValue nw = web_node_wrap(f, n);
    JS_SetPropertyStr(f->ctx, ev_obj, "currentTarget", JS_DupValue(f->ctx, nw));
    invoke_listeners(f, &n->listeners, nw, ev_obj, type, false, &sp, &si);
    if (!web_frame_alive(v, fid)) return;
    if (!si) invoke_on_prop(f, nw, ev_obj, type);
    if (!web_frame_alive(v, fid)) return;
    JS_FreeValue(f->ctx, nw);
  }
  if (bubbles && !sp) {
    JS_SetPropertyStr(f->ctx, ev_obj, "currentTarget", JS_DupValue(f->ctx, f->win_obj));
    invoke_listeners(f, &f->win_listeners, f->win_obj, ev_obj, type, false, &sp, &si);
    if (!web_frame_alive(v, fid)) return;
    if (!si) invoke_on_prop(f, f->win_obj, ev_obj, type);
    if (!web_frame_alive(v, fid)) return;
  }
  JS_FreeValue(f->ctx, target_js);
  JS_FreeCString(f->ctx, type);
  web_drain_jobs(v);
}

void web_dom_dispatch_simple(web_frame *f, web_node *target, const char *type, bool bubbles)
{
  if (!f || !f->ctx || !f->view || !target || !type) return;
  ccord_web_view *v = f->view;
  uint32_t fid = f->id;
  JSValue ev = web_js_make_event(f->ctx, type, bubbles, false);
  web_dom_dispatch_event(f, target, ev);
  if (web_frame_alive(v, fid)) JS_FreeValue(f->ctx, ev);
}

void web_dom_fire_mutations(ccord_web_view *v, web_node *target, const char *type,
                            web_node *added, web_node *removed, const char *attr_name)
{
  static int s_mut_depth = 0;
  if (!v || !target || !target->frame || s_mut_depth >= 8) return;
  // Snapshot matching observers first so callbacks adding/removing observers or frames are safe.
  web_frame *frames[32];
  uint32_t fids[32];
  JSValue cbs[32];
  size_t n = 0;
  for (web_mut_obs *mo = v->mut_obs; mo && n < 32u; mo = mo->next) {
    if (!mo->target || !mo->frame || mo->frame != target->frame) continue;
    bool hit = (mo->target == target);
    if (!hit && mo->subtree) {
      for (web_node *p = target->parent; p; p = p->parent) {
        if (p == mo->target) { hit = true; break; }
      }
    }
    if (!hit) continue;
    if (strcmp(type, "childList") == 0 && !mo->child_list) continue;
    if (strcmp(type, "attributes") == 0 && !mo->attributes) continue;
    frames[n] = mo->frame;
    fids[n] = mo->frame->id;
    cbs[n] = JS_DupValue(mo->frame->ctx, mo->cb);
    n++;
  }
  s_mut_depth++;
  for (size_t i = 0; i < n; i++) {
    if (!web_frame_alive(v, fids[i])) continue;
    web_frame *f = frames[i];
    JSContext *ctx = f->ctx;
    JSValue rec = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, rec, "type", JS_NewString(ctx, type));
    JS_SetPropertyStr(ctx, rec, "target", web_node_wrap(f, target));
    JSValue add_a = JS_NewArray(ctx);
    if (added) JS_SetPropertyUint32(ctx, add_a, 0, web_node_wrap(f, added));
    JSValue rem_a = JS_NewArray(ctx);
    if (removed) JS_SetPropertyUint32(ctx, rem_a, 0, web_node_wrap(f, removed));
    JS_SetPropertyStr(ctx, rec, "addedNodes", add_a);
    JS_SetPropertyStr(ctx, rec, "removedNodes", rem_a);
    JS_SetPropertyStr(ctx, rec, "attributeName", attr_name ? JS_NewString(ctx, attr_name) : JS_NULL);
    JSValue recs = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, recs, 0, rec);
    JSValue args[1] = {recs};
    JSValue r = JS_Call(ctx, cbs[i], JS_UNDEFINED, 1, args);
    if (web_frame_alive(v, fids[i])) {
      if (JS_IsException(r)) web_js_log_exception(f, "MutationObserver");
      JS_FreeValue(ctx, r);
      JS_FreeValue(ctx, recs);
      JS_FreeValue(ctx, cbs[i]);
    }
  }
  s_mut_depth--;
}

// ---- JS Node / Element / Document bindings ----

web_node *web_node_unwrap(JSContext *ctx, JSValueConst val)
{
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || !f->view) return nullptr;
  return JS_GetOpaque(val, f->view->cls_node);
}

static void maybe_reactivate_parent(web_node *p)
{
  if (!p || p->type != WEB_NODE_ELEMENT || !p->tag || !node_is_connected(p)) return;
  if (strcmp(p->tag, "style") == 0) activate_style_or_link(p);
  else if (strcmp(p->tag, "script") == 0) activate_script(p);
}

static JSValue node_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *p = web_node_unwrap(ctx, this_val);
  web_node *c = argc > 0 ? web_node_unwrap(ctx, argv[0]) : nullptr;
  if (!p || !c || !p->frame) return argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_NULL;
  ccord_web_view *v = p->frame->view;
  uint32_t fid = p->frame->id;
  bool is_frag = (c->type == WEB_NODE_FRAGMENT);
  web_node *prev_last = p->last_child;
  web_node_append(p, c);
  if (is_frag) {
    for (web_node *ch = prev_last ? prev_last->next_sib : p->first_child; ch; ) {
      web_node *next = ch->next_sib;
      activate_subtree_on_insert(ch);
      if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
      ch = next;
    }
  } else {
    activate_subtree_on_insert(c);
    if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
  }
  maybe_reactivate_parent(p);
  if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
  web_dom_fire_mutations(v, p, "childList", c, nullptr, nullptr);
  return JS_DupValue(ctx, argv[0]);
}

static JSValue node_insert_before(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *p = web_node_unwrap(ctx, this_val);
  web_node *c = argc > 0 ? web_node_unwrap(ctx, argv[0]) : nullptr;
  web_node *ref = (argc > 1 && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) ? web_node_unwrap(ctx, argv[1]) : nullptr;
  if (!p || !c || !p->frame) return argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_NULL;
  ccord_web_view *v = p->frame->view;
  uint32_t fid = p->frame->id;
  bool is_frag = (c->type == WEB_NODE_FRAGMENT);
  web_node *prev_before = (ref && ref->parent == p) ? ref->prev_sib : p->last_child;
  web_node_insert_before(p, c, ref);
  if (is_frag) {
    for (web_node *ch = prev_before ? prev_before->next_sib : p->first_child; ch && ch != ref; ) {
      web_node *next = ch->next_sib;
      activate_subtree_on_insert(ch);
      if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
      ch = next;
    }
  } else {
    activate_subtree_on_insert(c);
    if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
  }
  maybe_reactivate_parent(p);
  if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
  web_dom_fire_mutations(v, p, "childList", c, nullptr, nullptr);
  return JS_DupValue(ctx, argv[0]);
}

static JSValue node_replace_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *p = web_node_unwrap(ctx, this_val);
  web_node *nc = argc > 0 ? web_node_unwrap(ctx, argv[0]) : nullptr;
  web_node *oc = argc > 1 ? web_node_unwrap(ctx, argv[1]) : nullptr;
  if (!p || !nc || !oc || oc->parent != p || !p->frame) return argc > 1 ? JS_DupValue(ctx, argv[1]) : JS_NULL;
  ccord_web_view *v = p->frame->view;
  uint32_t fid = p->frame->id;
  bool is_frag = (nc->type == WEB_NODE_FRAGMENT);
  web_node *prev_before = oc->prev_sib;
  web_node_insert_before(p, nc, oc);
  web_node_remove(oc);
  if (is_frag) {
    for (web_node *ch = prev_before ? prev_before->next_sib : p->first_child; ch; ) {
      web_node *next = ch->next_sib;
      activate_subtree_on_insert(ch);
      if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
      ch = next;
    }
  } else {
    activate_subtree_on_insert(nc);
    if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
  }
  maybe_reactivate_parent(p);
  if (!web_frame_alive(v, fid)) return JS_UNDEFINED;
  web_dom_fire_mutations(v, p, "childList", nc, oc, nullptr);
  return JS_DupValue(ctx, argv[1]);
}

static JSValue node_has_child_nodes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  return JS_NewBool(ctx, n && n->first_child != nullptr);
}

static JSValue node_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *p = web_node_unwrap(ctx, this_val);
  web_node *c = argc > 0 ? web_node_unwrap(ctx, argv[0]) : nullptr;
  if (!p || !c || c->parent != p) return argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_NULL;
  web_node_remove(c);
  web_dom_fire_mutations(p->frame->view, p, "childList", nullptr, c, nullptr);
  return JS_DupValue(ctx, argv[0]);
}

static JSValue node_remove_self(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  if (n && n->parent) {
    web_node *p = n->parent;
    web_node_remove(n);
    web_dom_fire_mutations(p->frame->view, p, "childList", nullptr, n, nullptr);
  }
  return JS_UNDEFINED;
}

static JSValue node_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *a = web_node_unwrap(ctx, this_val);
  web_node *b = argc > 0 ? web_node_unwrap(ctx, argv[0]) : nullptr;
  for (web_node *cur = b; cur; cur = cur->parent) {
    if (cur == a) return JS_TRUE;
  }
  return JS_FALSE;
}

static web_node *clone_subtree(web_frame *f, const web_node *src, bool deep)
{
  web_node *dst = web_node_create(f, src->type, src->tag);
  if (!dst) return nullptr;
  if (src->text) dst->text = ccord_strdup(src->text);
  for (size_t i = 0; i < src->nattrs; i++) web_node_set_attr(dst, src->attrs[i].name, src->attrs[i].value);
  for (size_t i = 0; i < src->ninline; i++) web_node_set_inline_style(dst, src->inline_props[i].name, src->inline_props[i].value);
  if (deep) {
    for (const web_node *c = src->first_child; c; c = c->next_sib) {
      web_node *cc = clone_subtree(f, c, true);
      if (cc) web_node_append(dst, cc);
    }
  }
  return dst;
}

static JSValue node_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n) return JS_NULL;
  bool deep = argc > 0 && JS_ToBool(ctx, argv[0]) > 0;
  return web_node_wrap(n->frame, clone_subtree(n->frame, n, deep));
}

static JSValue node_get_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NULL;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_EXCEPTION;
  const char *v = web_node_get_attr(n, name);
  JS_FreeCString(ctx, name);
  return v ? JS_NewString(ctx, v) : JS_NULL;
}

static JSValue node_set_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 2) return JS_UNDEFINED;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_EXCEPTION;
  const char *val = JS_ToCString(ctx, argv[1]);
  if (!val) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
  web_node_set_attr(n, name, val);
  web_dom_fire_mutations(n->frame->view, n, "attributes", nullptr, nullptr, name);
  JS_FreeCString(ctx, name);
  JS_FreeCString(ctx, val);
  return JS_UNDEFINED;
}

static JSValue node_rem_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_UNDEFINED;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_EXCEPTION;
  web_node_remove_attr(n, name);
  web_dom_fire_mutations(n->frame->view, n, "attributes", nullptr, nullptr, name);
  JS_FreeCString(ctx, name);
  return JS_UNDEFINED;
}

static JSValue node_has_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_FALSE;
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_EXCEPTION;
  bool has = web_node_get_attr(n, name) != nullptr;
  JS_FreeCString(ctx, name);
  return JS_NewBool(ctx, has);
}

static JSValue node_get_attr_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  if (argc < 2) return JS_NULL;
  return node_get_attr(ctx, this_val, 1, &argv[1]);
}

static JSValue node_set_attr_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  if (argc < 3) return JS_UNDEFINED;
  return node_set_attr(ctx, this_val, 2, &argv[1]);
}

static JSValue node_rem_attr_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  if (argc < 2) return JS_UNDEFINED;
  return node_rem_attr(ctx, this_val, 1, &argv[1]);
}

static JSValue node_get_attr_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  JSValue arr = JS_NewArray(ctx);
  if (!n) return arr;
  for (size_t i = 0; i < n->nattrs; i++) {
    JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, n->attrs[i].name));
  }
  return arr;
}

static JSValue node_has_attrs(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  return JS_NewBool(ctx, n && n->nattrs > 0);
}

static JSValue node_add_el(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 2) return JS_UNDEFINED;
  const char *type = JS_ToCString(ctx, argv[0]);
  if (!type) return JS_EXCEPTION;
  bool capture = false, once = false;
  if (argc > 2) {
    if (JS_IsBool(argv[2])) capture = JS_ToBool(ctx, argv[2]) > 0;
    else if (JS_IsObject(argv[2])) {
      JSValue cv = JS_GetPropertyStr(ctx, argv[2], "capture");
      JSValue ov = JS_GetPropertyStr(ctx, argv[2], "once");
      capture = JS_ToBool(ctx, cv) > 0;
      once = JS_ToBool(ctx, ov) > 0;
      JS_FreeValue(ctx, cv); JS_FreeValue(ctx, ov);
    }
  }
  add_listener(ctx, &n->listeners, type, argv[1], capture, once);
  JS_FreeCString(ctx, type);
  return JS_UNDEFINED;
}

static JSValue node_rem_el(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 2) return JS_UNDEFINED;
  const char *type = JS_ToCString(ctx, argv[0]);
  if (!type) return JS_EXCEPTION;
  bool capture = (argc > 2 && JS_IsBool(argv[2])) ? JS_ToBool(ctx, argv[2]) > 0 : false;
  remove_listener(ctx, &n->listeners, type, argv[1], capture);
  JS_FreeCString(ctx, type);
  return JS_UNDEFINED;
}

static JSValue node_dispatch_ev(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_FALSE;
  web_dom_dispatch_event(n->frame, n, argv[0]);
  return JS_TRUE;
}

static JSValue node_qs(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NULL;
  const char *sel = JS_ToCString(ctx, argv[0]);
  if (!sel) return JS_EXCEPTION;
  web_node *hit = web_dom_query_one(n, sel);
  JS_FreeCString(ctx, sel);
  return hit ? web_node_wrap(n->frame, hit) : JS_NULL;
}

static JSValue node_qsa(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NewArray(ctx);
  const char *sel = JS_ToCString(ctx, argv[0]);
  if (!sel) return JS_EXCEPTION;
  JSValue arr = JS_NewArray(ctx);
  web_node *hits[256];
  size_t nh = web_dom_query_all(n, sel, hits, 256u);
  JS_FreeCString(ctx, sel);
  for (size_t i = 0; i < nh; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, web_node_wrap(n->frame, hits[i]));
  return arr;
}

static web_node *find_by_id_walk(web_node *n, const char *id)
{
  for (web_node *c = n->first_child; c; c = c->next_sib) {
    if (c->type == WEB_NODE_ELEMENT) {
      const char *v = web_node_get_attr(c, "id");
      if (v && strcmp(v, id) == 0) return c;
    }
    web_node *hit = find_by_id_walk(c, id);
    if (hit) return hit;
  }
  return nullptr;
}

static JSValue node_get_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NULL;
  const char *id = JS_ToCString(ctx, argv[0]);
  if (!id) return JS_EXCEPTION;
  if (!id[0]) { JS_FreeCString(ctx, id); return JS_NULL; }
  web_node *hit = find_by_id_walk(n, id);
  JS_FreeCString(ctx, id);
  return hit ? web_node_wrap(n->frame, hit) : JS_NULL;
}

static JSValue node_get_by_tag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NewArray(ctx);
  const char *tag = JS_ToCString(ctx, argv[0]);
  if (!tag) return JS_EXCEPTION;
  JSValue arr = JS_NewArray(ctx);
  web_node *hits[512];
  size_t nh = web_dom_query_all(n, tag, hits, 512u);
  JS_FreeCString(ctx, tag);
  for (size_t i = 0; i < nh; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, web_node_wrap(n->frame, hits[i]));
  return arr;
}

static bool has_all_classes(const char *elem_cls, const char *wanted)
{
  if (!elem_cls || !wanted) return false;
  const char *p = wanted;
  bool any = false;
  while (*p) {
    while (*p && isspace((unsigned char)*p)) p++;
    const char *st = p;
    while (*p && !isspace((unsigned char)*p)) p++;
    if (p > st) {
      any = true;
      if (!class_list_contains(elem_cls, st, (size_t)(p - st))) return false;
    }
  }
  return any;
}

static void walk_by_class(web_node *n, const char *cls, web_node **out, size_t *cnt, size_t cap)
{
  for (web_node *c = n->first_child; c; c = c->next_sib) {
    if (c->type == WEB_NODE_ELEMENT && has_all_classes(web_node_get_attr(c, "class"), cls)) {
      if (*cnt < cap) out[(*cnt)++] = c;
    }
    walk_by_class(c, cls, out, cnt, cap);
  }
}

static JSValue node_get_by_class(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NewArray(ctx);
  const char *cls = JS_ToCString(ctx, argv[0]);
  if (!cls) return JS_EXCEPTION;
  JSValue arr = JS_NewArray(ctx);
  web_node *hits[512];
  size_t nh = 0;
  walk_by_class(n, cls, hits, &nh, 512u);
  JS_FreeCString(ctx, cls);
  for (size_t i = 0; i < nh; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, web_node_wrap(n->frame, hits[i]));
  return arr;
}

static void walk_by_name(web_node *n, const char *name, web_node **out, size_t *cnt, size_t cap)
{
  for (web_node *c = n->first_child; c; c = c->next_sib) {
    if (c->type == WEB_NODE_ELEMENT) {
      const char *v = web_node_get_attr(c, "name");
      if (v && strcmp(v, name) == 0 && *cnt < cap) out[(*cnt)++] = c;
    }
    walk_by_name(c, name, out, cnt, cap);
  }
}

static JSValue node_get_by_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NewArray(ctx);
  const char *name = JS_ToCString(ctx, argv[0]);
  if (!name) return JS_EXCEPTION;
  JSValue arr = JS_NewArray(ctx);
  web_node *hits[256];
  size_t nh = 0;
  walk_by_name(n, name, hits, &nh, 256u);
  JS_FreeCString(ctx, name);
  for (size_t i = 0; i < nh; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, web_node_wrap(n->frame, hits[i]));
  return arr;
}

static JSValue node_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_FALSE;
  const char *sel = JS_ToCString(ctx, argv[0]);
  if (!sel) return JS_EXCEPTION;
  bool ok = web_dom_matches_selector(n, sel);
  JS_FreeCString(ctx, sel);
  return JS_NewBool(ctx, ok);
}

static JSValue node_closest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NULL;
  const char *sel = JS_ToCString(ctx, argv[0]);
  if (!sel) return JS_EXCEPTION;
  web_node *hit = nullptr;
  for (web_node *cur = n; cur; cur = cur->parent) {
    if (cur->type == WEB_NODE_ELEMENT && web_dom_matches_selector(cur, sel)) { hit = cur; break; }
  }
  JS_FreeCString(ctx, sel);
  return hit ? web_node_wrap(n->frame, hit) : JS_NULL;
}

static JSValue node_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  if (n && n->frame) web_frame_ensure_layout(n->frame, n->frame->view->measure_r);
  ccord_rect r = (n && n->box_valid) ? n->box : (ccord_rect){0, 0, 0, 0};
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, (double)r.x));
  JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, (double)r.y));
  JS_SetPropertyStr(ctx, o, "left", JS_NewFloat64(ctx, (double)r.x));
  JS_SetPropertyStr(ctx, o, "top", JS_NewFloat64(ctx, (double)r.y));
  JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, (double)r.w));
  JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, (double)r.h));
  JS_SetPropertyStr(ctx, o, "right", JS_NewFloat64(ctx, (double)(r.x + r.w)));
  JS_SetPropertyStr(ctx, o, "bottom", JS_NewFloat64(ctx, (double)(r.y + r.h)));
  return o;
}

static JSValue node_get_context(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || !n->tag || strcmp(n->tag, "canvas") != 0 || argc < 1) return JS_NULL;
  const char *kind = JS_ToCString(ctx, argv[0]);
  if (!kind) return JS_EXCEPTION;
  bool is_2d = strcmp(kind, "2d") == 0;
  JS_FreeCString(ctx, kind);
  return is_2d ? web_canvas_get_context_2d(n->frame, n) : JS_NULL;
}

static JSValue node_to_data_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || !n->tag || strcmp(n->tag, "canvas") != 0) return JS_NewString(ctx, "data:,");
  if (!n->canvas) {
    int w = 300, h = 150;
    const char *ws = web_node_get_attr(n, "width"), *hs = web_node_get_attr(n, "height");
    if (ws && atoi(ws) > 0) w = atoi(ws);
    if (hs && atoi(hs) > 0) h = atoi(hs);
    n->canvas = web_canvas_create(w, h);
  }
  const char *mime = (argc > 0 && JS_IsString(argv[0])) ? JS_ToCString(ctx, argv[0]) : nullptr;
  ccord_buf out;
  ccord_buf_init(&out, 4u * 1024u * 1024u);
  bool ok = web_canvas_to_data_url(n->canvas, mime, &out);
  if (mime) JS_FreeCString(ctx, mime);
  JSValue res = ok ? JS_NewStringLen(ctx, out.data, out.len) : JS_NewString(ctx, "data:,");
  ccord_buf_free(&out);
  return res;
}

static JSValue node_style_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 1) return JS_NewString(ctx, "");
  const char *prop = JS_ToCString(ctx, argv[0]);
  if (!prop) return JS_EXCEPTION;
  const char *v = web_node_get_inline_style(n, prop);
  JS_FreeCString(ctx, prop);
  return JS_NewString(ctx, v ? v : "");
}

static JSValue node_style_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 2) return JS_UNDEFINED;
  const char *prop = JS_ToCString(ctx, argv[0]);
  if (!prop) return JS_EXCEPTION;
  const char *val = JS_ToCString(ctx, argv[1]);
  if (!val) { JS_FreeCString(ctx, prop); return JS_EXCEPTION; }
  web_node_set_inline_style(n, prop, val);
  JS_FreeCString(ctx, prop);
  JS_FreeCString(ctx, val);
  return JS_UNDEFINED;
}

static JSValue node_style_css_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n) return JS_NewString(ctx, "");
  ccord_buf b;
  ccord_buf_init(&b, 65536);
  for (size_t i = 0; i < n->ninline; i++) {
    if (i > 0) (void)ccord_buf_append_str(&b, " ");
    (void)ccord_buf_append_str(&b, n->inline_props[i].name);
    (void)ccord_buf_append_str(&b, ": ");
    (void)ccord_buf_append_str(&b, n->inline_props[i].value);
    (void)ccord_buf_append_str(&b, ";");
  }
  JSValue r = JS_NewStringLen(ctx, b.data ? b.data : "", b.len);
  ccord_buf_free(&b);
  return r;
}

static JSValue node_style_insert_rule(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || !n->frame || argc < 1) return JS_NewInt32(ctx, 0);
  size_t len = 0;
  const char *rule = JS_ToCStringLen(ctx, &len, argv[0]);
  if (!rule) return JS_EXCEPTION;
  if (len > 0) {
    web_css_parse_sheet(&n->frame->sheet, rule, len);
    n->frame->style_dirty = true;
    n->frame->layout_dirty = true;
    web_mark_dirty(n->frame->view);
  }
  JS_FreeCString(ctx, rule);
  return JS_NewUint32(ctx, (uint32_t)(n->frame->sheet.nrules > 0 ? n->frame->sheet.nrules - 1u : 0u));
}

static JSValue node_insert_adj_el(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  web_node *el = argc > 1 ? web_node_unwrap(ctx, argv[1]) : nullptr;
  if (!n || !el || argc < 2) return JS_NULL;
  const char *pos = JS_ToCString(ctx, argv[0]);
  if (!pos) return JS_EXCEPTION;
  if (ascii_ieq(pos, "beforebegin") && n->parent) {
    web_node_insert_before(n->parent, el, n);
  } else if (ascii_ieq(pos, "afterbegin")) {
    web_node_insert_before(n, el, n->first_child);
  } else if (ascii_ieq(pos, "beforeend")) {
    web_node_append(n, el);
  } else if (ascii_ieq(pos, "afterend") && n->parent) {
    web_node_insert_before(n->parent, el, n->next_sib);
  }
  JS_FreeCString(ctx, pos);
  activate_subtree_on_insert(el);
  return JS_DupValue(ctx, argv[1]);
}

static JSValue node_insert_adj_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n || argc < 2) return JS_UNDEFINED;
  const char *pos = JS_ToCString(ctx, argv[0]);
  if (!pos) return JS_EXCEPTION;
  size_t hlen = 0;
  const char *html = JS_ToCStringLen(ctx, &hlen, argv[1]);
  if (!html) { JS_FreeCString(ctx, pos); return JS_EXCEPTION; }
  web_node *frag = web_node_create(n->frame, WEB_NODE_FRAGMENT, "#document-fragment");
  if (frag) {
    (void)web_dom_parse_fragment_ex(n->frame, frag, html, hlen, true);
    if (ascii_ieq(pos, "beforebegin") && n->parent) {
      web_node_insert_before(n->parent, frag, n);
    } else if (ascii_ieq(pos, "afterbegin")) {
      web_node_insert_before(n, frag, n->first_child);
    } else if (ascii_ieq(pos, "beforeend")) {
      web_node_append(n, frag);
    } else if (ascii_ieq(pos, "afterend") && n->parent) {
      web_node_insert_before(n->parent, frag, n->next_sib);
    }
    if (node_is_connected(n)) {
      web_node *root_p = (ascii_ieq(pos, "beforebegin") || ascii_ieq(pos, "afterend")) ? n->parent : n;
      if (root_p && root_p->frame) {
        ccord_web_view *v = root_p->frame->view;
        uint32_t fid = root_p->frame->id;
        for (web_node *c = root_p->first_child; c; ) {
          web_node *next = c->next_sib;
          activate_subtree_on_insert(c);
          if (!web_frame_alive(v, fid)) break;
          c = next;
        }
      }
    }
  }
  JS_FreeCString(ctx, pos);
  JS_FreeCString(ctx, html);
  return JS_UNDEFINED;
}

static JSValue doc_parse_frag_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f) return JS_NULL;
  size_t len = 0;
  const char *s = nullptr;
  if (argc > 0) {
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
  }
  web_node *frag = web_node_create(f, WEB_NODE_FRAGMENT, "#document-fragment");
  if (frag && s) {
    (void)web_dom_parse_fragment_ex(f, frag, s, len, true);
  }
  if (s) JS_FreeCString(ctx, s);
  return frag ? web_node_wrap(f, frag) : JS_NULL;
}

static JSValue node_click(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n) return JS_UNDEFINED;
  if (n->tag && strcmp(n->tag, "input") == 0) {
    const char *t = web_node_get_attr(n, "type");
    if (t && ascii_ieq(t, "checkbox")) {
      n->checked = !n->checked;
      web_mark_dirty(n->frame->view);
    }
  }
  web_dom_dispatch_simple(n->frame, n, "click", true);
  return JS_UNDEFINED;
}

static JSValue node_focus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  if (n && n->frame) {
    n->frame->focused_node = n;
    web_dom_dispatch_simple(n->frame, n, "focus", false);
  }
  return JS_UNDEFINED;
}

static JSValue node_blur(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  web_node *n = web_node_unwrap(ctx, this_val);
  if (n && n->frame && n->frame->focused_node == n) {
    n->frame->focused_node = nullptr;
    web_dom_dispatch_simple(n->frame, n, "blur", false);
  }
  return JS_UNDEFINED;
}

// Document factory methods
static JSValue doc_create_el(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 1) return JS_NULL;
  const char *tag = JS_ToCString(ctx, argv[0]);
  if (!tag) return JS_EXCEPTION;
  web_node *el = web_node_create(f, WEB_NODE_ELEMENT, tag);
  JS_FreeCString(ctx, tag);
  return el ? web_node_wrap(f, el) : JS_NULL;
}

static JSValue doc_create_el_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 2) return JS_NULL;
  const char *ns = JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) ? nullptr : JS_ToCString(ctx, argv[0]);
  if (!JS_IsNull(argv[0]) && !JS_IsUndefined(argv[0]) && !ns) return JS_EXCEPTION;
  const char *tag = JS_ToCString(ctx, argv[1]);
  if (!tag) { if (ns) JS_FreeCString(ctx, ns); return JS_EXCEPTION; }
  web_node *el = web_node_create(f, WEB_NODE_ELEMENT, tag);
  if (el && ns) el->ns = ccord_strdup(ns);
  if (ns) JS_FreeCString(ctx, ns);
  JS_FreeCString(ctx, tag);
  return el ? web_node_wrap(f, el) : JS_NULL;
}

static JSValue doc_create_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f) return JS_NULL;
  const char *s = nullptr;
  if (argc > 0) {
    s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
  }
  web_node *tn = web_node_create(f, WEB_NODE_TEXT, "#text");
  if (tn && s) tn->text = ccord_strdup(s);
  if (s) JS_FreeCString(ctx, s);
  return tn ? web_node_wrap(f, tn) : JS_NULL;
}

static JSValue doc_create_comment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f) return JS_NULL;
  const char *s = nullptr;
  if (argc > 0) {
    s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_EXCEPTION;
  }
  web_node *cn = web_node_create(f, WEB_NODE_COMMENT, "#comment");
  if (cn && s) cn->text = ccord_strdup(s);
  if (s) JS_FreeCString(ctx, s);
  return cn ? web_node_wrap(f, cn) : JS_NULL;
}

static JSValue doc_create_frag(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val; (void)argc; (void)argv;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f) return JS_NULL;
  return web_node_wrap(f, web_node_create(f, WEB_NODE_FRAGMENT, "#document-fragment"));
}

static JSValue doc_create_event(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val; (void)argc; (void)argv;
  return web_js_make_event(ctx, "", false, false);
}

// Node property getters/setters via magic enum
enum {
  NP_NODETYPE = 0, NP_TAGNAME, NP_NODENAME, NP_PARENT, NP_FIRST, NP_LAST, NP_NEXT, NP_PREV,
  NP_FIRST_EL, NP_LAST_EL, NP_NEXT_EL, NP_PREV_EL, NP_CHILD_EL_CNT, NP_IS_CONNECTED,
  NP_NODE_VALUE, NP_CHILDREN, NP_CHILDNODES, NP_TEXTCONTENT, NP_INNERHTML, NP_OUTERHTML,
  NP_ID, NP_CLASSNAME, NP_SRC, NP_HREF,
  NP_WIDTH, NP_HEIGHT, NP_NAT_WIDTH, NP_NAT_HEIGHT, NP_COMPLETE, NP_VALUE, NP_CHECKED,
  NP_OFFSET_W, NP_OFFSET_H, NP_CLIENT_W, NP_CLIENT_H, NP_OFFSET_L, NP_OFFSET_T, NP_OFFSET_PARENT, NP_ZERO_NUM,
  NP_CONTENT_WIN, NP_CONTENT_DOC, NP_OWNER_DOC, NP_ATTRIBUTES,
  NP_DOC_HEAD, NP_DOC_BODY, NP_DOC_EL, NP_DOC_CUR_SCRIPT, NP_DOC_READY_STATE,
  NP_DOC_URL, NP_DOC_DOMAIN, NP_DOC_REFERRER, NP_DOC_SCRIPTS, NP_DOC_DEFAULT_VIEW, NP_DOC_ACTIVE_EL,
  NP_ATTR_TYPE, NP_ATTR_NAME, NP_ATTR_TITLE, NP_ATTR_REL, NP_ATTR_ASYNC, NP_ATTR_DEFER,
  NP_ATTR_CROSSORIGIN, NP_ATTR_SANDBOX, NP_ATTR_ALLOW, NP_ATTR_FRAMEBORDER, NP_ATTR_SCROLLING,
  NP_ATTR_ROLE, NP_ATTR_TABINDEX, NP_ATTR_DISABLED, NP_ATTR_PLACEHOLDER, NP_ATTR_NONCE,
  NP_ATTR_REFPOLICY, NP_ATTR_ALT, NP_ATTR_ARIALABEL,
};

static const char *reflected_attr_name(int magic)
{
  switch (magic) {
    case NP_ATTR_TYPE: return "type";
    case NP_ATTR_NAME: return "name";
    case NP_ATTR_TITLE: return "title";
    case NP_ATTR_REL: return "rel";
    case NP_ATTR_ASYNC: return "async";
    case NP_ATTR_DEFER: return "defer";
    case NP_ATTR_CROSSORIGIN: return "crossorigin";
    case NP_ATTR_SANDBOX: return "sandbox";
    case NP_ATTR_ALLOW: return "allow";
    case NP_ATTR_FRAMEBORDER: return "frameborder";
    case NP_ATTR_SCROLLING: return "scrolling";
    case NP_ATTR_ROLE: return "role";
    case NP_ATTR_TABINDEX: return "tabindex";
    case NP_ATTR_DISABLED: return "disabled";
    case NP_ATTR_PLACEHOLDER: return "placeholder";
    case NP_ATTR_NONCE: return "nonce";
    case NP_ATTR_REFPOLICY: return "referrerpolicy";
    case NP_ATTR_ALT: return "alt";
    case NP_ATTR_ARIALABEL: return "aria-label";
    default: return nullptr;
  }
}

static void serialize_one_node(const web_node *c, ccord_buf *b);

static void serialize_html(const web_node *n, ccord_buf *b)
{
  for (const web_node *c = n->first_child; c; c = c->next_sib) {
    serialize_one_node(c, b);
  }
}

static void serialize_one_node(const web_node *c, ccord_buf *b)
{
  if (c->type == WEB_NODE_TEXT) {
    if (c->text) (void)ccord_buf_append_str(b, c->text);
  } else if (c->type == WEB_NODE_ELEMENT && c->tag) {
    (void)ccord_buf_append_str(b, "<");
    (void)ccord_buf_append_str(b, c->tag);
    for (size_t i = 0; i < c->nattrs; i++) {
      (void)ccord_buf_append_str(b, " ");
      (void)ccord_buf_append_str(b, c->attrs[i].name);
      (void)ccord_buf_append_str(b, "=\"");
      (void)ccord_buf_append_str(b, c->attrs[i].value);
      (void)ccord_buf_append_str(b, "\"");
    }
    (void)ccord_buf_append_str(b, ">");
    serialize_html(c, b);
    (void)ccord_buf_append_str(b, "</");
    (void)ccord_buf_append_str(b, c->tag);
    (void)ccord_buf_append_str(b, ">");
  }
}

static JSValue node_prop_get(JSContext *ctx, JSValueConst this_val, int magic)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n) return JS_UNDEFINED;
  web_frame *f = n->frame;
  switch (magic) {
    case NP_NODETYPE: return JS_NewInt32(ctx, (int32_t)n->type);
    case NP_TAGNAME: {
      if (n->type != WEB_NODE_ELEMENT || !n->tag) return JS_UNDEFINED;
      char *u = dup_upper(n->tag);
      JSValue r = u ? JS_NewString(ctx, u) : JS_UNDEFINED;
      ccord_free(u);
      return r;
    }
    case NP_NODENAME: {
      if (n->type == WEB_NODE_ELEMENT && n->tag) {
        char *u = dup_upper(n->tag);
        JSValue r = u ? JS_NewString(ctx, u) : JS_UNDEFINED;
        ccord_free(u);
        return r;
      }
      return JS_NewString(ctx, n->tag ? n->tag : "");
    }
    case NP_PARENT: return n->parent ? web_node_wrap(f, n->parent) : JS_NULL;
    case NP_FIRST: return n->first_child ? web_node_wrap(f, n->first_child) : JS_NULL;
    case NP_LAST: return n->last_child ? web_node_wrap(f, n->last_child) : JS_NULL;
    case NP_NEXT: return n->next_sib ? web_node_wrap(f, n->next_sib) : JS_NULL;
    case NP_PREV: return n->prev_sib ? web_node_wrap(f, n->prev_sib) : JS_NULL;
    case NP_FIRST_EL: {
      for (web_node *c = n->first_child; c; c = c->next_sib) {
        if (c->type == WEB_NODE_ELEMENT) return web_node_wrap(f, c);
      }
      return JS_NULL;
    }
    case NP_LAST_EL: {
      for (web_node *c = n->last_child; c; c = c->prev_sib) {
        if (c->type == WEB_NODE_ELEMENT) return web_node_wrap(f, c);
      }
      return JS_NULL;
    }
    case NP_NEXT_EL: {
      for (web_node *c = n->next_sib; c; c = c->next_sib) {
        if (c->type == WEB_NODE_ELEMENT) return web_node_wrap(f, c);
      }
      return JS_NULL;
    }
    case NP_PREV_EL: {
      for (web_node *c = n->prev_sib; c; c = c->prev_sib) {
        if (c->type == WEB_NODE_ELEMENT) return web_node_wrap(f, c);
      }
      return JS_NULL;
    }
    case NP_CHILD_EL_CNT: {
      uint32_t cnt = 0;
      for (web_node *c = n->first_child; c; c = c->next_sib) {
        if (c->type == WEB_NODE_ELEMENT) cnt++;
      }
      return JS_NewUint32(ctx, cnt);
    }
    case NP_IS_CONNECTED: return JS_NewBool(ctx, node_is_connected(n));
    case NP_NODE_VALUE:
      if (n->type == WEB_NODE_TEXT || n->type == WEB_NODE_COMMENT) return JS_NewString(ctx, n->text ? n->text : "");
      return JS_NULL;
    case NP_CHILDREN: {
      JSValue a = JS_NewArray(ctx);
      uint32_t idx = 0;
      for (web_node *c = n->first_child; c; c = c->next_sib) {
        if (c->type == WEB_NODE_ELEMENT) JS_SetPropertyUint32(ctx, a, idx++, web_node_wrap(f, c));
      }
      return a;
    }
    case NP_CHILDNODES: {
      JSValue a = JS_NewArray(ctx);
      uint32_t idx = 0;
      for (web_node *c = n->first_child; c; c = c->next_sib) {
        JS_SetPropertyUint32(ctx, a, idx++, web_node_wrap(f, c));
      }
      return a;
    }
    case NP_TEXTCONTENT: {
      ccord_buf b;
      ccord_buf_init(&b, 2u * 1024u * 1024u);
      collect_text(n, &b);
      JSValue r = JS_NewStringLen(ctx, b.data ? b.data : "", b.len);
      ccord_buf_free(&b);
      return r;
    }
    case NP_INNERHTML: {
      ccord_buf b;
      ccord_buf_init(&b, 2u * 1024u * 1024u);
      serialize_html(n, &b);
      JSValue r = JS_NewStringLen(ctx, b.data ? b.data : "", b.len);
      ccord_buf_free(&b);
      return r;
    }
    case NP_OUTERHTML: {
      ccord_buf b;
      ccord_buf_init(&b, 2u * 1024u * 1024u);
      serialize_one_node(n, &b);
      JSValue r = JS_NewStringLen(ctx, b.data ? b.data : "", b.len);
      ccord_buf_free(&b);
      return r;
    }
    case NP_ID: { const char *v = web_node_get_attr(n, "id"); return JS_NewString(ctx, v ? v : ""); }
    case NP_CLASSNAME: { const char *v = web_node_get_attr(n, "class"); return JS_NewString(ctx, v ? v : ""); }
    case NP_SRC: {
      const char *v = web_node_get_attr(n, "src");
      if (!v || !v[0]) return JS_NewString(ctx, "");
      web_url *u = f ? web_url_alloc_parse(v, &f->url) : nullptr;
      if (u) { JSValue r = JS_NewString(ctx, u->href); ccord_free(u); return r; }
      return JS_NewString(ctx, v);
    }
    case NP_HREF: {
      const char *v = web_node_get_attr(n, "href");
      if (!v || !v[0]) return JS_NewString(ctx, "");
      web_url *u = f ? web_url_alloc_parse(v, &f->url) : nullptr;
      if (u) { JSValue r = JS_NewString(ctx, u->href); ccord_free(u); return r; }
      return JS_NewString(ctx, v);
    }
    case NP_WIDTH: {
      if (n->canvas) return JS_NewInt32(ctx, web_canvas_width(n->canvas));
      if (n->img && n->img->loaded) return JS_NewInt32(ctx, n->img->w);
      const char *v = web_node_get_attr(n, "width");
      return JS_NewInt32(ctx, v ? atoi(v) : (n->tag && strcmp(n->tag, "canvas") == 0 ? 300 : (int)n->box.w));
    }
    case NP_HEIGHT: {
      if (n->canvas) return JS_NewInt32(ctx, web_canvas_height(n->canvas));
      if (n->img && n->img->loaded) return JS_NewInt32(ctx, n->img->h);
      const char *v = web_node_get_attr(n, "height");
      return JS_NewInt32(ctx, v ? atoi(v) : (n->tag && strcmp(n->tag, "canvas") == 0 ? 150 : (int)n->box.h));
    }
    case NP_NAT_WIDTH: return JS_NewInt32(ctx, (n->img && n->img->loaded) ? n->img->w : 0);
    case NP_NAT_HEIGHT: return JS_NewInt32(ctx, (n->img && n->img->loaded) ? n->img->h : 0);
    case NP_COMPLETE: return JS_NewBool(ctx, !n->img || !n->img->loading);
    case NP_VALUE: {
      if (n->text) return JS_NewString(ctx, n->text);
      const char *v = web_node_get_attr(n, "value");
      return JS_NewString(ctx, v ? v : "");
    }
    case NP_CHECKED: return JS_NewBool(ctx, n->checked);
    case NP_OFFSET_W: web_frame_ensure_layout(f, f->view->measure_r); return JS_NewFloat64(ctx, (double)n->box.w);
    case NP_OFFSET_H: web_frame_ensure_layout(f, f->view->measure_r); return JS_NewFloat64(ctx, (double)n->box.h);
    case NP_CLIENT_W:
      if (n->tag && strcmp(n->tag, "html") == 0 && f) return JS_NewFloat64(ctx, (double)f->viewport_w);
      web_frame_ensure_layout(f, f->view->measure_r);
      return JS_NewFloat64(ctx, (double)n->box.w);
    case NP_CLIENT_H:
      if (n->tag && strcmp(n->tag, "html") == 0 && f) return JS_NewFloat64(ctx, (double)f->viewport_h);
      web_frame_ensure_layout(f, f->view->measure_r);
      return JS_NewFloat64(ctx, (double)n->box.h);
    case NP_OFFSET_L: web_frame_ensure_layout(f, f->view->measure_r); return JS_NewFloat64(ctx, (double)n->box.x);
    case NP_OFFSET_T: web_frame_ensure_layout(f, f->view->measure_r); return JS_NewFloat64(ctx, (double)n->box.y);
    case NP_OFFSET_PARENT: return (n->parent && n->parent->type == WEB_NODE_ELEMENT) ? web_node_wrap(f, n->parent) : JS_NULL;
    case NP_ZERO_NUM: return JS_NewInt32(ctx, 0);
    case NP_CONTENT_WIN: {
      if (!n->child_frame && n->tag && strcmp(n->tag, "iframe") == 0) activate_iframe(n);
      if (!n->child_frame) return JS_NULL;
      return web_js_get_frame_proxy(f, n->child_frame->id);
    }
    case NP_CONTENT_DOC:
      if (n->child_frame && strcmp(n->child_frame->url.origin, f->url.origin) == 0 && n->child_frame->doc) {
        return web_node_wrap(f, n->child_frame->doc);
      }
      return JS_NULL;
    case NP_OWNER_DOC: return f->doc ? web_node_wrap(f, f->doc) : JS_NULL;
    case NP_ATTRIBUTES: {
      JSValue a = JS_NewArray(ctx);
      for (size_t i = 0; i < n->nattrs; i++) {
        JSValue it = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, it, "name", JS_NewString(ctx, n->attrs[i].name));
        JS_SetPropertyStr(ctx, it, "value", JS_NewString(ctx, n->attrs[i].value));
        JS_SetPropertyUint32(ctx, a, (uint32_t)i, it);
      }
      return a;
    }
    case NP_DOC_HEAD: return f->head ? web_node_wrap(f, f->head) : JS_NULL;
    case NP_DOC_BODY: return f->body ? web_node_wrap(f, f->body) : JS_NULL;
    case NP_DOC_EL: {
      web_node *h = f->doc ? web_dom_query_one(f->doc, "html") : nullptr;
      return h ? web_node_wrap(f, h) : JS_NULL;
    }
    case NP_DOC_CUR_SCRIPT: return f->current_script ? web_node_wrap(f, f->current_script) : JS_NULL;
    case NP_DOC_READY_STATE:
      return JS_NewString(ctx, f->loaded ? "complete" : (f->dom_content_loaded ? "interactive" : "loading"));
    case NP_DOC_URL: return JS_NewString(ctx, f->url.href);
    case NP_DOC_DOMAIN: return JS_NewString(ctx, f->url.hostname);
    case NP_DOC_REFERRER: return JS_NewString(ctx, f->parent ? f->parent->url.href : "");
    case NP_DOC_SCRIPTS: {
      JSValue a = JS_NewArray(ctx);
      if (f->doc) {
        web_node *hits[128];
        size_t nh = web_dom_query_all(f->doc, "script", hits, 128u);
        for (size_t i = 0; i < nh; i++) JS_SetPropertyUint32(ctx, a, (uint32_t)i, web_node_wrap(f, hits[i]));
      }
      return a;
    }
    case NP_DOC_DEFAULT_VIEW: return JS_DupValue(ctx, f->win_obj);
    case NP_DOC_ACTIVE_EL:
      if (f->focused_node) return web_node_wrap(f, f->focused_node);
      return f->body ? web_node_wrap(f, f->body) : JS_NULL;
    case NP_ATTR_ASYNC:
    case NP_ATTR_DEFER:
    case NP_ATTR_DISABLED: {
      const char *an = reflected_attr_name(magic);
      return JS_NewBool(ctx, an && web_node_get_attr(n, an) != nullptr);
    }
    case NP_ATTR_TABINDEX: {
      const char *v = web_node_get_attr(n, "tabindex");
      return JS_NewInt32(ctx, v ? atoi(v) : -1);
    }
    default: {
      const char *an = reflected_attr_name(magic);
      if (an) {
        const char *v = web_node_get_attr(n, an);
        return JS_NewString(ctx, v ? v : "");
      }
      return JS_UNDEFINED;
    }
  }
}

static JSValue node_prop_set(JSContext *ctx, JSValueConst this_val, JSValueConst val, int magic)
{
  web_node *n = web_node_unwrap(ctx, this_val);
  if (!n) return JS_UNDEFINED;
  if (magic == NP_CHECKED) {
    n->checked = JS_ToBool(ctx, val) > 0;
    web_mark_dirty(n->frame->view);
    return JS_UNDEFINED;
  }
  if (magic == NP_ATTR_ASYNC || magic == NP_ATTR_DEFER || magic == NP_ATTR_DISABLED) {
    const char *an = reflected_attr_name(magic);
    if (an) {
      if (JS_ToBool(ctx, val) > 0) web_node_set_attr(n, an, "");
      else web_node_remove_attr(n, an);
    }
    return JS_UNDEFINED;
  }
  if (magic == NP_ZERO_NUM) return JS_UNDEFINED;
  const char *s = JS_ToCString(ctx, val);
  if (!s) return JS_EXCEPTION;
  switch (magic) {
    case NP_NODE_VALUE:
      if (n->type == WEB_NODE_TEXT || n->type == WEB_NODE_COMMENT) {
        ccord_free(n->text);
        n->text = ccord_strdup(s ? s : "");
        maybe_reactivate_parent(n->parent);
        n->frame->layout_dirty = true;
        web_mark_dirty(n->frame->view);
      }
      break;
    case NP_TEXTCONTENT:
      if (n->type == WEB_NODE_TEXT || n->type == WEB_NODE_COMMENT) {
        ccord_free(n->text);
        n->text = ccord_strdup(s ? s : "");
        maybe_reactivate_parent(n->parent);
      } else {
        while (n->first_child) web_node_remove(n->first_child);
        if (s && s[0]) {
          web_node *tn = web_node_create(n->frame, WEB_NODE_TEXT, "#text");
          if (tn) { tn->text = ccord_strdup(s); web_node_append(n, tn); }
        }
        if (n->tag && strcmp(n->tag, "style") == 0 && node_is_connected(n)) activate_style_or_link(n);
        else if (n->tag && strcmp(n->tag, "script") == 0 && node_is_connected(n)) activate_script(n);
      }
      n->frame->layout_dirty = true;
      web_mark_dirty(n->frame->view);
      break;
    case NP_INNERHTML:
      if (n->tag && (strcmp(n->tag, "style") == 0 || strcmp(n->tag, "script") == 0)) {
        while (n->first_child) web_node_remove(n->first_child);
        if (s && s[0]) {
          web_node *tn = web_node_create(n->frame, WEB_NODE_TEXT, "#text");
          if (tn) { tn->text = ccord_strdup(s); web_node_append(n, tn); }
        }
        if (node_is_connected(n)) {
          if (strcmp(n->tag, "style") == 0) activate_style_or_link(n);
          else activate_script(n);
        }
      } else {
        (void)web_dom_parse_fragment(n->frame, n, s ? s : "", s ? strlen(s) : 0);
      }
      break;
    case NP_ID: web_node_set_attr(n, "id", s ? s : ""); break;
    case NP_CLASSNAME: web_node_set_attr(n, "class", s ? s : ""); break;
    case NP_SRC: web_node_set_attr(n, "src", s ? s : ""); break;
    case NP_HREF: web_node_set_attr(n, "href", s ? s : ""); break;
    case NP_WIDTH: web_node_set_attr(n, "width", s ? s : "300"); break;
    case NP_HEIGHT: web_node_set_attr(n, "height", s ? s : "150"); break;
    case NP_VALUE:
      ccord_free(n->text);
      n->text = ccord_strdup(s ? s : "");
      web_mark_dirty(n->frame->view);
      break;
    default: {
      const char *an = reflected_attr_name(magic);
      if (an) web_node_set_attr(n, an, s ? s : "");
      break;
    }
  }
  if (s) JS_FreeCString(ctx, s);
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry k_node_proto[] = {
  JS_CFUNC_DEF("appendChild", 1, node_append_child),
  JS_CFUNC_DEF("insertBefore", 2, node_insert_before),
  JS_CFUNC_DEF("replaceChild", 2, node_replace_child),
  JS_CFUNC_DEF("removeChild", 1, node_remove_child),
  JS_CFUNC_DEF("remove", 0, node_remove_self),
  JS_CFUNC_DEF("contains", 1, node_contains),
  JS_CFUNC_DEF("hasChildNodes", 0, node_has_child_nodes),
  JS_CFUNC_DEF("cloneNode", 1, node_clone),
  JS_CFUNC_DEF("getAttribute", 1, node_get_attr),
  JS_CFUNC_DEF("setAttribute", 2, node_set_attr),
  JS_CFUNC_DEF("removeAttribute", 1, node_rem_attr),
  JS_CFUNC_DEF("hasAttribute", 1, node_has_attr),
  JS_CFUNC_DEF("getAttributeNS", 2, node_get_attr_ns),
  JS_CFUNC_DEF("setAttributeNS", 3, node_set_attr_ns),
  JS_CFUNC_DEF("removeAttributeNS", 2, node_rem_attr_ns),
  JS_CFUNC_DEF("getAttributeNames", 0, node_get_attr_names),
  JS_CFUNC_DEF("hasAttributes", 0, node_has_attrs),
  JS_CFUNC_DEF("addEventListener", 3, node_add_el),
  JS_CFUNC_DEF("removeEventListener", 3, node_rem_el),
  JS_CFUNC_DEF("dispatchEvent", 1, node_dispatch_ev),
  JS_CFUNC_DEF("querySelector", 1, node_qs),
  JS_CFUNC_DEF("querySelectorAll", 1, node_qsa),
  JS_CFUNC_DEF("getElementById", 1, node_get_by_id),
  JS_CFUNC_DEF("getElementsByTagName", 1, node_get_by_tag),
  JS_CFUNC_DEF("getElementsByClassName", 1, node_get_by_class),
  JS_CFUNC_DEF("getElementsByName", 1, node_get_by_name),
  JS_CFUNC_DEF("matches", 1, node_matches),
  JS_CFUNC_DEF("closest", 1, node_closest),
  JS_CFUNC_DEF("insertAdjacentElement", 2, node_insert_adj_el),
  JS_CFUNC_DEF("insertAdjacentHTML", 2, node_insert_adj_html),
  JS_CFUNC_DEF("getBoundingClientRect", 0, node_rect),
  JS_CFUNC_DEF("getContext", 2, node_get_context),
  JS_CFUNC_DEF("toDataURL", 2, node_to_data_url),
  JS_CFUNC_DEF("__styleGet", 1, node_style_get),
  JS_CFUNC_DEF("__styleSet", 2, node_style_set),
  JS_CFUNC_DEF("__styleCssText", 0, node_style_css_text),
  JS_CFUNC_DEF("__styleInsertRule", 1, node_style_insert_rule),
  JS_CFUNC_DEF("__parseFragment", 1, doc_parse_frag_html),
  JS_CFUNC_DEF("click", 0, node_click),
  JS_CFUNC_DEF("focus", 0, node_focus),
  JS_CFUNC_DEF("blur", 0, node_blur),
  JS_CFUNC_DEF("createElement", 1, doc_create_el),
  JS_CFUNC_DEF("createElementNS", 2, doc_create_el_ns),
  JS_CFUNC_DEF("createTextNode", 1, doc_create_text),
  JS_CFUNC_DEF("createComment", 1, doc_create_comment),
  JS_CFUNC_DEF("createDocumentFragment", 0, doc_create_frag),
  JS_CFUNC_DEF("createEvent", 1, doc_create_event),
  JS_CGETSET_MAGIC_DEF("nodeType", node_prop_get, nullptr, NP_NODETYPE),
  JS_CGETSET_MAGIC_DEF("tagName", node_prop_get, nullptr, NP_TAGNAME),
  JS_CGETSET_MAGIC_DEF("nodeName", node_prop_get, nullptr, NP_NODENAME),
  JS_CGETSET_MAGIC_DEF("nodeValue", node_prop_get, node_prop_set, NP_NODE_VALUE),
  JS_CGETSET_MAGIC_DEF("data", node_prop_get, node_prop_set, NP_NODE_VALUE),
  JS_CGETSET_MAGIC_DEF("parentNode", node_prop_get, nullptr, NP_PARENT),
  JS_CGETSET_MAGIC_DEF("parentElement", node_prop_get, nullptr, NP_PARENT),
  JS_CGETSET_MAGIC_DEF("firstChild", node_prop_get, nullptr, NP_FIRST),
  JS_CGETSET_MAGIC_DEF("lastChild", node_prop_get, nullptr, NP_LAST),
  JS_CGETSET_MAGIC_DEF("nextSibling", node_prop_get, nullptr, NP_NEXT),
  JS_CGETSET_MAGIC_DEF("previousSibling", node_prop_get, nullptr, NP_PREV),
  JS_CGETSET_MAGIC_DEF("firstElementChild", node_prop_get, nullptr, NP_FIRST_EL),
  JS_CGETSET_MAGIC_DEF("lastElementChild", node_prop_get, nullptr, NP_LAST_EL),
  JS_CGETSET_MAGIC_DEF("nextElementSibling", node_prop_get, nullptr, NP_NEXT_EL),
  JS_CGETSET_MAGIC_DEF("previousElementSibling", node_prop_get, nullptr, NP_PREV_EL),
  JS_CGETSET_MAGIC_DEF("childElementCount", node_prop_get, nullptr, NP_CHILD_EL_CNT),
  JS_CGETSET_MAGIC_DEF("isConnected", node_prop_get, nullptr, NP_IS_CONNECTED),
  JS_CGETSET_MAGIC_DEF("children", node_prop_get, nullptr, NP_CHILDREN),
  JS_CGETSET_MAGIC_DEF("childNodes", node_prop_get, nullptr, NP_CHILDNODES),
  JS_CGETSET_MAGIC_DEF("textContent", node_prop_get, node_prop_set, NP_TEXTCONTENT),
  JS_CGETSET_MAGIC_DEF("innerText", node_prop_get, node_prop_set, NP_TEXTCONTENT),
  JS_CGETSET_MAGIC_DEF("innerHTML", node_prop_get, node_prop_set, NP_INNERHTML),
  JS_CGETSET_MAGIC_DEF("outerHTML", node_prop_get, nullptr, NP_OUTERHTML),
  JS_CGETSET_MAGIC_DEF("id", node_prop_get, node_prop_set, NP_ID),
  JS_CGETSET_MAGIC_DEF("className", node_prop_get, node_prop_set, NP_CLASSNAME),
  JS_CGETSET_MAGIC_DEF("src", node_prop_get, node_prop_set, NP_SRC),
  JS_CGETSET_MAGIC_DEF("href", node_prop_get, node_prop_set, NP_HREF),
  JS_CGETSET_MAGIC_DEF("width", node_prop_get, node_prop_set, NP_WIDTH),
  JS_CGETSET_MAGIC_DEF("height", node_prop_get, node_prop_set, NP_HEIGHT),
  JS_CGETSET_MAGIC_DEF("naturalWidth", node_prop_get, nullptr, NP_NAT_WIDTH),
  JS_CGETSET_MAGIC_DEF("naturalHeight", node_prop_get, nullptr, NP_NAT_HEIGHT),
  JS_CGETSET_MAGIC_DEF("complete", node_prop_get, nullptr, NP_COMPLETE),
  JS_CGETSET_MAGIC_DEF("value", node_prop_get, node_prop_set, NP_VALUE),
  JS_CGETSET_MAGIC_DEF("checked", node_prop_get, node_prop_set, NP_CHECKED),
  JS_CGETSET_MAGIC_DEF("offsetWidth", node_prop_get, nullptr, NP_OFFSET_W),
  JS_CGETSET_MAGIC_DEF("offsetHeight", node_prop_get, nullptr, NP_OFFSET_H),
  JS_CGETSET_MAGIC_DEF("clientWidth", node_prop_get, nullptr, NP_CLIENT_W),
  JS_CGETSET_MAGIC_DEF("clientHeight", node_prop_get, nullptr, NP_CLIENT_H),
  JS_CGETSET_MAGIC_DEF("scrollWidth", node_prop_get, nullptr, NP_OFFSET_W),
  JS_CGETSET_MAGIC_DEF("scrollHeight", node_prop_get, nullptr, NP_OFFSET_H),
  JS_CGETSET_MAGIC_DEF("offsetLeft", node_prop_get, nullptr, NP_OFFSET_L),
  JS_CGETSET_MAGIC_DEF("offsetTop", node_prop_get, nullptr, NP_OFFSET_T),
  JS_CGETSET_MAGIC_DEF("offsetParent", node_prop_get, nullptr, NP_OFFSET_PARENT),
  JS_CGETSET_MAGIC_DEF("clientTop", node_prop_get, nullptr, NP_ZERO_NUM),
  JS_CGETSET_MAGIC_DEF("clientLeft", node_prop_get, nullptr, NP_ZERO_NUM),
  JS_CGETSET_MAGIC_DEF("scrollTop", node_prop_get, node_prop_set, NP_ZERO_NUM),
  JS_CGETSET_MAGIC_DEF("scrollLeft", node_prop_get, node_prop_set, NP_ZERO_NUM),
  JS_CGETSET_MAGIC_DEF("contentWindow", node_prop_get, nullptr, NP_CONTENT_WIN),
  JS_CGETSET_MAGIC_DEF("contentDocument", node_prop_get, nullptr, NP_CONTENT_DOC),
  JS_CGETSET_MAGIC_DEF("ownerDocument", node_prop_get, nullptr, NP_OWNER_DOC),
  JS_CGETSET_MAGIC_DEF("attributes", node_prop_get, nullptr, NP_ATTRIBUTES),
  JS_CGETSET_MAGIC_DEF("head", node_prop_get, nullptr, NP_DOC_HEAD),
  JS_CGETSET_MAGIC_DEF("body", node_prop_get, nullptr, NP_DOC_BODY),
  JS_CGETSET_MAGIC_DEF("documentElement", node_prop_get, nullptr, NP_DOC_EL),
  JS_CGETSET_MAGIC_DEF("currentScript", node_prop_get, nullptr, NP_DOC_CUR_SCRIPT),
  JS_CGETSET_MAGIC_DEF("readyState", node_prop_get, nullptr, NP_DOC_READY_STATE),
  JS_CGETSET_MAGIC_DEF("URL", node_prop_get, nullptr, NP_DOC_URL),
  JS_CGETSET_MAGIC_DEF("documentURI", node_prop_get, nullptr, NP_DOC_URL),
  JS_CGETSET_MAGIC_DEF("domain", node_prop_get, nullptr, NP_DOC_DOMAIN),
  JS_CGETSET_MAGIC_DEF("referrer", node_prop_get, nullptr, NP_DOC_REFERRER),
  JS_CGETSET_MAGIC_DEF("scripts", node_prop_get, nullptr, NP_DOC_SCRIPTS),
  JS_CGETSET_MAGIC_DEF("defaultView", node_prop_get, nullptr, NP_DOC_DEFAULT_VIEW),
  JS_CGETSET_MAGIC_DEF("activeElement", node_prop_get, nullptr, NP_DOC_ACTIVE_EL),
  JS_CGETSET_MAGIC_DEF("type", node_prop_get, node_prop_set, NP_ATTR_TYPE),
  JS_CGETSET_MAGIC_DEF("name", node_prop_get, node_prop_set, NP_ATTR_NAME),
  JS_CGETSET_MAGIC_DEF("title", node_prop_get, node_prop_set, NP_ATTR_TITLE),
  JS_CGETSET_MAGIC_DEF("rel", node_prop_get, node_prop_set, NP_ATTR_REL),
  JS_CGETSET_MAGIC_DEF("async", node_prop_get, node_prop_set, NP_ATTR_ASYNC),
  JS_CGETSET_MAGIC_DEF("defer", node_prop_get, node_prop_set, NP_ATTR_DEFER),
  JS_CGETSET_MAGIC_DEF("crossOrigin", node_prop_get, node_prop_set, NP_ATTR_CROSSORIGIN),
  JS_CGETSET_MAGIC_DEF("sandbox", node_prop_get, node_prop_set, NP_ATTR_SANDBOX),
  JS_CGETSET_MAGIC_DEF("allow", node_prop_get, node_prop_set, NP_ATTR_ALLOW),
  JS_CGETSET_MAGIC_DEF("frameBorder", node_prop_get, node_prop_set, NP_ATTR_FRAMEBORDER),
  JS_CGETSET_MAGIC_DEF("scrolling", node_prop_get, node_prop_set, NP_ATTR_SCROLLING),
  JS_CGETSET_MAGIC_DEF("role", node_prop_get, node_prop_set, NP_ATTR_ROLE),
  JS_CGETSET_MAGIC_DEF("tabIndex", node_prop_get, node_prop_set, NP_ATTR_TABINDEX),
  JS_CGETSET_MAGIC_DEF("disabled", node_prop_get, node_prop_set, NP_ATTR_DISABLED),
  JS_CGETSET_MAGIC_DEF("placeholder", node_prop_get, node_prop_set, NP_ATTR_PLACEHOLDER),
  JS_CGETSET_MAGIC_DEF("nonce", node_prop_get, node_prop_set, NP_ATTR_NONCE),
  JS_CGETSET_MAGIC_DEF("referrerPolicy", node_prop_get, node_prop_set, NP_ATTR_REFPOLICY),
  JS_CGETSET_MAGIC_DEF("alt", node_prop_get, node_prop_set, NP_ATTR_ALT),
  JS_CGETSET_MAGIC_DEF("ariaLabel", node_prop_get, node_prop_set, NP_ATTR_ARIALABEL),
};

void web_dom_register_classes(ccord_web_view *v)
{
  JS_NewClassID(v->rt, &v->cls_node);
  JSClassDef cd = {.class_name = "Node"};
  JS_NewClass(v->rt, v->cls_node, &cd);
}

void web_dom_init_frame(web_frame *f)
{
  JSContext *ctx = f->ctx;
  JSValue proto = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, proto, k_node_proto, (int)(sizeof k_node_proto / sizeof k_node_proto[0]));
  JS_SetClassProto(ctx, f->view->cls_node, proto);

  // Enhance Node.prototype with style Proxy, style.sheet, classList, dataset, DOM helpers (split <4KB for C23 string limit)
  static const char k_dom_helpers_1[] =
    "(function(P){"
    "  Object.defineProperty(P, 'style', { configurable: true, get() {"
    "    if (!this._styleProxy) {"
    "      const el = this;"
    "      this._styleProxy = new Proxy({"
    "        setProperty(k, v){ el.__styleSet(k, v == null ? '' : String(v)); },"
    "        getPropertyValue(k){ return el.__styleGet(k); },"
    "        removeProperty(k){ const o = el.__styleGet(k); el.__styleSet(k, ''); return o; }"
    "      }, {"
    "        get(t, p){ if (p in t) return t[p]; if (p === 'cssText') return el.__styleCssText(); return typeof p === 'string' ? el.__styleGet(p) : undefined; },"
    "        set(t, p, v){ if (p === 'cssText') { el.setAttribute('style', v == null ? '' : String(v)); return true; } if (typeof p === 'string') el.__styleSet(p, v == null ? '' : String(v)); return true; }"
    "      });"
    "    }"
    "    return this._styleProxy;"
    "  }});"
    "  Object.defineProperty(P, 'sheet', { configurable: true, get() {"
    "    if (this.tagName !== 'STYLE' && this.tagName !== 'LINK') return null;"
    "    if (!this._sheet) {"
    "      const el = this; const rules = [];"
    "      this._sheet = { cssRules: rules, rules: rules, ownerNode: el,"
    "        insertRule(r, i){ const idx = el.__styleInsertRule(String(r)); rules.push({ cssText: String(r) }); return typeof i === 'number' ? i : idx; },"
    "        deleteRule(i){ if (i >= 0 && i < rules.length) rules.splice(i, 1); }"
    "      };"
    "    }"
    "    return this._sheet;"
    "  }});"
    "  Object.defineProperty(P, 'classList', { configurable: true, get() {"
    "    const el = this;"
    "    const list = () => (el.className || '').trim().split(/\\s+/).filter(Boolean);"
    "    return {"
    "      get length(){ return list().length; },"
    "      item(i){ return list()[i] ?? null; },"
    "      contains(c){ return list().includes(c); },"
    "      add(...cs){ const s = new Set(list()); cs.forEach(c => { if (c) s.add(c); }); el.className = [...s].join(' '); },"
    "      remove(...cs){ const s = new Set(list()); cs.forEach(c => s.delete(c)); el.className = [...s].join(' '); },"
    "      replace(o, n){ const s = new Set(list()); if (!s.has(o)) return false; s.delete(o); if (n) s.add(n); el.className = [...s].join(' '); return true; },"
    "      toggle(c, f){ const has = list().includes(c); const on = f === undefined ? !has : !!f; if (on) this.add(c); else this.remove(c); return on; },"
    "      [Symbol.iterator](){ return list()[Symbol.iterator](); }"
    "    };"
    "  }});"
    "  Object.defineProperty(P, 'dataset', { configurable: true, get() {"
    "    const el = this;"
    "    return new Proxy({}, {"
    "      get(_, k){ if (typeof k !== 'string') return undefined; const a = 'data-' + k.replace(/[A-Z]/g, m => '-' + m.toLowerCase()); const v = el.getAttribute(a); return v === null ? undefined : v; },"
    "      set(_, k, v){ if (typeof k === 'string') { const a = 'data-' + k.replace(/[A-Z]/g, m => '-' + m.toLowerCase()); el.setAttribute(a, String(v)); } return true; }"
    "    });"
    "  }});"
    "})(Object.getPrototypeOf(__ccordProtoNode));";

  static const char k_dom_helpers_2[] =
    "(function(P){"
    "  const toNode = (self, x) => typeof x === 'string' ? (self.ownerDocument || self).createTextNode(x) : x;"
    "  P.append = function(...xs){ for (const x of xs) this.appendChild(toNode(this, x)); };"
    "  P.prepend = function(...xs){ const f = this.firstChild; for (const x of xs) this.insertBefore(toNode(this, x), f); };"
    "  P.after = function(...xs){ if (!this.parentNode) return; const r = this.nextSibling; for (const x of xs) this.parentNode.insertBefore(toNode(this, x), r); };"
    "  P.before = function(...xs){ if (!this.parentNode) return; for (const x of xs) this.parentNode.insertBefore(toNode(this, x), this); };"
    "  P.replaceWith = function(...xs){ if (!this.parentNode) return; for (const x of xs) this.parentNode.insertBefore(toNode(this, x), this); this.remove(); };"
    "  P.insertAdjacentText = function(pos, txt){ const t = (this.ownerDocument || this).createTextNode(String(txt)); this.insertAdjacentElement(pos, t); };"
    "  P.getClientRects = function(){ return [this.getBoundingClientRect()]; };"
    "  P.scrollIntoView = function(){}; P.scrollTo = function(){};"
    "  P.attachShadow = function(){ this.host = this; return this; };"
    "  P.getBBox = function(){ const r = this.getBoundingClientRect(); return { x:r.x, y:r.y, width:r.width, height:r.height }; };"
    "  P.getTotalLength = function(){ return 100; };"
    "  P.hasFocus = function(){ return true; };"
    "  P.createRange = function(){ const d = this; return { selectNodeContents(){}, setStart(){}, setEnd(){}, collapse(){}, cloneRange(){ return this; },"
    "    getBoundingClientRect(){ return {x:0,y:0,left:0,top:0,width:0,height:0,right:0,bottom:0}; },"
    "    createContextualFragment(html){ return d.__parseFragment(String(html)); } }; };"
    "  P.implementation = { createHTMLDocument(title){ const d = document.createElement('div'); d.title = title || ''; return d; }, hasFeature(){ return true; } };"
    "  Object.defineProperty(P, 'styleSheets', { configurable: true, get(){ return this.nodeType === 9 ? [...this.querySelectorAll('style')].map(s => s.sheet).filter(Boolean) : []; } });"
    "  Object.defineProperty(P, 'adoptedStyleSheets', { configurable: true,"
    "    get(){ return this._adoptedSheets || (this._adoptedSheets = []); },"
    "    set(v){ this._adoptedSheets = Array.isArray(v) ? v : []; for (const s of this._adoptedSheets) { if (s && s._css) this.__styleInsertRule(s._css); } }"
    "  });"
    "  P.fonts = { ready: Promise.resolve(), status: 'loaded', check(){ return true; }, load(){ return Promise.resolve([]); }, add(){}, addEventListener(){}, removeEventListener(){} };"
    "  P.visibilityState = 'visible'; P.hidden = false; P.compatMode = 'CSS1Compat'; P.characterSet = 'UTF-8'; P.charset = 'UTF-8';"
    "  let _cookies = new Map();"
    "  Object.defineProperty(P, 'cookie', { configurable: true,"
    "    get(){ return [..._cookies.entries()].map(([k,v]) => k + '=' + v).join('; '); },"
    "    set(v){ const p = String(v).split(';')[0] || ''; const i = p.indexOf('='); if (i > 0) _cookies.set(p.slice(0, i).trim(), p.slice(i + 1).trim()); }"
    "  });"
    "})(Object.getPrototypeOf(__ccordProtoNode));";

  static const char k_dom_helpers_3[] =
    "(function(P){"
    "  P.decode = function(){ return Promise.resolve(); };"
    "  P.animate = function(){"
    "    const anim = { finished: Promise.resolve(), ready: Promise.resolve(), currentTime: 0, playState: 'finished', playbackRate: 1,"
    "      onfinish: null, oncancel: null, cancel(){}, finish(){}, pause(){}, play(){}, reverse(){}, commitStyles(){},"
    "      addEventListener(t, fn){ if (t === 'finish' && typeof fn === 'function') Promise.resolve().then(() => fn({ type: 'finish', target: anim })); },"
    "      removeEventListener(){} };"
    "    return anim;"
    "  };"
    "  P.getAnimations = function(){ return []; };"
    "  P.setPointerCapture = function(){}; P.releasePointerCapture = function(){}; P.hasPointerCapture = function(){ return false; };"
    "  P.toBlob = function(cb, type){ const u = this.toDataURL(type || 'image/png'); const b64 = (u.split(',')[1] || '');"
    "    let arr = new Uint8Array(0); try { const bin = atob(b64); arr = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i); } catch(_){}"
    "    if (typeof cb === 'function') setTimeout(() => cb(new Blob([arr], { type: type || 'image/png' })), 0); };"
    "  P.toggleAttribute = function(k, f){ const h = this.hasAttribute(k); const on = f === undefined ? !h : !!f; if (on) this.setAttribute(k, ''); else this.removeAttribute(k); return on; };"
    "  P.getRootNode = function(){ let c = this; while (c.parentNode) c = c.parentNode; return c; };"
    "  P.isSameNode = function(o){ return this === o; }; P.isEqualNode = function(o){ return this === o; };"
    "  P.compareDocumentPosition = function(o){ if (this === o) return 0; if (this.contains && this.contains(o)) return 20; if (o && o.contains && o.contains(this)) return 10; return 4; };"
    "  P.normalize = function(){}; P.scroll = function(){}; P.scrollBy = function(){};"
    "  P.elementFromPoint = function(){ return this.body || this; }; P.elementsFromPoint = function(){ return this.body ? [this.body] : []; };"
    "  P.createTreeWalker = function(root){ return { root, currentNode: root, nextNode(){ return null; }, previousNode(){ return null; } }; };"
    "  P.createNodeIterator = function(root){ return { root, referenceNode: root, nextNode(){ return null; } }; };"
    "  P.execCommand = function(){ return false; }; P.queryCommandSupported = function(){ return false; };"
    "  P.select = function(){}; P.setSelectionRange = function(){};"
    "  P.submit = function(){}; P.reset = function(){}; P.checkValidity = function(){ return true; }; P.reportValidity = function(){ return true; };"
    "})(Object.getPrototypeOf(__ccordProtoNode));";

  // Seed a temporary prototype probe node, run helper attach, then clean up global probe
  web_node *doc = web_node_create(f, WEB_NODE_DOCUMENT, "#document");
  f->doc = doc;
  JSValue doc_js = web_node_wrap(f, doc);
  f->doc_obj = JS_DupValue(ctx, doc_js);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "__ccordProtoNode", JS_DupValue(ctx, doc_js));
  JS_SetPropertyStr(ctx, global, "document", doc_js);
  JSValue rv1 = JS_Eval(ctx, k_dom_helpers_1, sizeof k_dom_helpers_1 - 1u, "<dom-init-1>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(rv1)) web_js_log_exception(f, "dom-init-1");
  JS_FreeValue(ctx, rv1);
  JSValue rv2 = JS_Eval(ctx, k_dom_helpers_2, sizeof k_dom_helpers_2 - 1u, "<dom-init-2>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(rv2)) web_js_log_exception(f, "dom-init-2");
  JS_FreeValue(ctx, rv2);
  JSValue rv3 = JS_Eval(ctx, k_dom_helpers_3, sizeof k_dom_helpers_3 - 1u, "<dom-init-3>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(rv3)) web_js_log_exception(f, "dom-init-3");
  JS_FreeValue(ctx, rv3);
  JSAtom probe_atom = JS_NewAtom(ctx, "__ccordProtoNode");
  (void)JS_DeleteProperty(ctx, global, probe_atom, 0);
  JS_FreeAtom(ctx, probe_atom);
  JS_FreeValue(ctx, global);
}

JSValue web_node_wrap(web_frame *f, web_node *n)
{
  if (!f || !f->ctx || !n) return JS_NULL;
  if (n->has_js_obj) return JS_DupValue(f->ctx, n->js_obj);
  JSValue obj = JS_NewObjectClass(f->ctx, f->view->cls_node);
  JS_SetOpaque(obj, n);
  n->js_obj = JS_DupValue(f->ctx, obj);
  n->has_js_obj = true;
  return obj;
}
