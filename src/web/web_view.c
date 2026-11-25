// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "web_internal.h"

#include <ccord/core/json.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Diagnostics & dirty state ----

void web_diag_append(ccord_web_view *v, const char *level, const char *fmt, ...)
{
  if (!v || !fmt) return;
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (level && (strcmp(level, "error") == 0 || strcmp(level, "warn") == 0)) {
    CCORD_LOG_W("web[%s]: %s", level, line);
  } else if (level && strcmp(level, "js-err") != 0) {
    CCORD_LOG_I("web[%s]: %s", level, line);
  }
  char entry[560];
  int en = snprintf(entry, sizeof entry, "[%s] %s\n", level ? level : "info", line);
  if (en <= 0) return;
  size_t elen = (size_t)en;
  if (elen >= WEB_DIAG_CAP - 1u) elen = WEB_DIAG_CAP - 1u;
  if (v->diag_len + elen >= WEB_DIAG_CAP) {
    size_t keep = (WEB_DIAG_CAP / 2u);
    if (v->diag_len > keep) {
      memmove(v->diag, v->diag + (v->diag_len - keep), keep);
      v->diag_len = keep;
    } else {
      v->diag_len = 0;
    }
  }
  memcpy(v->diag + v->diag_len, entry, elen);
  v->diag_len += elen;
  v->diag[v->diag_len] = '\0';
}

bool web_frame_alive(const ccord_web_view *v, uint32_t id)
{
  if (!v || id == 0) return false;
  for (size_t i = 0; i < v->nframes; i++) {
    if (v->frames[i] && v->frames[i]->id == id) return true;
  }
  return false;
}

static void host_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                           JSValueConst reason, bool is_handled, void *opaque)
{
  (void)promise;
  ccord_web_view *v = opaque;
  if (is_handled || !v || v->destroying) return;
  web_frame *f = JS_GetContextOpaque(ctx);
  const char *msg = JS_ToCString(ctx, reason);
  if (!msg) { JSValue e2 = JS_GetException(ctx); JS_FreeValue(ctx, e2); }
  JSValue st = JS_IsObject(reason) ? JS_GetPropertyStr(ctx, reason, "stack") : JS_UNDEFINED;
  const char *stack = JS_IsString(st) ? JS_ToCString(ctx, st) : nullptr;
  if (JS_IsString(st) && !stack) { JSValue e2 = JS_GetException(ctx); JS_FreeValue(ctx, e2); }
  uint32_t fid = f ? f->id : 0u;
  char fstack[420];
  web_js_format_stack(stack, fstack, sizeof fstack);
  web_diag_append(v, "js-err", "[f%u unhandledRejection] %s%s%s", fid,
                  msg ? msg : "(rejection)", fstack[0] ? " | " : "", fstack);
  CCORD_LOG_W("web[%u] unhandledRejection: %s%s%s", fid,
              msg ? msg : "(rejection)", fstack[0] ? " | " : "", fstack);
  if (msg) JS_FreeCString(ctx, msg);
  if (stack) JS_FreeCString(ctx, stack);
  JS_FreeValue(ctx, st);
}

void web_mark_dirty(ccord_web_view *v)
{
  if (v) v->dirty = true;
}

// ---- Timers, postMessage, blobs, storage ----

uint32_t web_add_timer(web_frame *f, JSValue fn, const char *code, uint32_t delay_ms,
                       bool is_interval, bool is_raf)
{
  if (!f || !f->view) return 0;
  ccord_web_view *v = f->view;
  for (size_t i = 0; i < WEB_MAX_TIMERS; i++) {
    if (!v->timers[i].active) {
      web_timer *t = &v->timers[i];
      t->id = ++v->next_timer_id;
      t->frame = f;
      t->fn = JS_IsUndefined(fn) ? JS_UNDEFINED : JS_DupValue(f->ctx, fn);
      t->code_str = code ? ccord_strdup(code) : nullptr;
      t->interval_ms = delay_ms;
      t->due_ms = (v->now_ms ? v->now_ms : ccord_clock_ms()) + delay_ms;
      t->is_interval = is_interval;
      t->is_raf = is_raf;
      t->active = true;
      return t->id;
    }
  }
  return 0;
}

void web_cancel_timer(ccord_web_view *v, uint32_t id)
{
  if (!v || id == 0) return;
  for (size_t i = 0; i < WEB_MAX_TIMERS; i++) {
    if (v->timers[i].active && v->timers[i].id == id) {
      if (v->timers[i].frame && v->timers[i].frame->ctx && !JS_IsUndefined(v->timers[i].fn)) {
        JS_FreeValue(v->timers[i].frame->ctx, v->timers[i].fn);
      }
      ccord_free(v->timers[i].code_str);
      memset(&v->timers[i], 0, sizeof v->timers[i]);
      return;
    }
  }
}

static void cancel_timers_for_frame(ccord_web_view *v, web_frame *f)
{
  for (size_t i = 0; i < WEB_MAX_TIMERS; i++) {
    if (v->timers[i].active && v->timers[i].frame == f) {
      if (f->ctx && !JS_IsUndefined(v->timers[i].fn)) JS_FreeValue(f->ctx, v->timers[i].fn);
      ccord_free(v->timers[i].code_str);
      memset(&v->timers[i], 0, sizeof v->timers[i]);
    }
  }
}

void web_enqueue_postmsg(ccord_web_view *v, web_frame *target, web_frame *source,
                         const char *origin, const char *payload, bool is_raw_str, uint32_t port_id)
{
  if (!v || !target || v->msg_count >= WEB_MAX_POSTMSG) return;
  web_postmsg *m = ccord_calloc(1, sizeof *m);
  if (!m) return;
  m->target = target;
  m->source = source;
  m->origin = ccord_strdup(origin ? origin : "");
  m->json_payload = ccord_strdup(payload ? payload : "null");
  m->is_raw_string = is_raw_str;
  m->port_id = port_id;
  if (v->msg_tail) v->msg_tail->next = m;
  else v->msg_head = m;
  v->msg_tail = m;
  v->msg_count++;
}

const char *web_blob_register(ccord_web_view *v, const uint8_t *data, size_t len, const char *mime)
{
  if (!v || v->nblobs >= WEB_MAX_BLOBS) return nullptr;
  web_blob_entry *be = &v->blobs[v->nblobs];
  uint32_t id = ++v->next_blob_id;
  snprintf(be->url, sizeof be->url, "blob:https://discord.com/ccord-%u", id);
  snprintf(be->mime, sizeof be->mime, "%s", mime ? mime : "application/octet-stream");
  be->data = ccord_malloc(len + 1u);
  if (!be->data) return nullptr;
  if (data && len > 0) memcpy(be->data, data, len);
  be->data[len] = '\0';
  be->len = len;
  v->nblobs++;
  return be->url;
}

const web_blob_entry *web_blob_find(const ccord_web_view *v, const char *url)
{
  if (!v || !url) return nullptr;
  for (size_t i = 0; i < v->nblobs; i++) {
    if (strcmp(v->blobs[i].url, url) == 0) return &v->blobs[i];
  }
  return nullptr;
}

const char *web_storage_get(const ccord_web_view *v, const char *origin, const char *key)
{
  if (!v || !origin || !key) return nullptr;
  for (web_storage_kv *kv = v->storage; kv; kv = kv->next) {
    if (strcmp(kv->origin, origin) == 0 && strcmp(kv->key, key) == 0) return kv->val;
  }
  return nullptr;
}

void web_storage_set(ccord_web_view *v, const char *origin, const char *key, const char *val)
{
  if (!v || !origin || !key || !val) return;
  for (web_storage_kv *kv = v->storage; kv; kv = kv->next) {
    if (strcmp(kv->origin, origin) == 0 && strcmp(kv->key, key) == 0) {
      char *nv = ccord_strdup(val);
      if (nv) { ccord_free(kv->val); kv->val = nv; }
      return;
    }
  }
  web_storage_kv *kv = ccord_calloc(1, sizeof *kv);
  if (!kv) return;
  kv->origin = ccord_strdup(origin);
  kv->key = ccord_strdup(key);
  kv->val = ccord_strdup(val);
  kv->next = v->storage;
  v->storage = kv;
}

void web_storage_remove(ccord_web_view *v, const char *origin, const char *key)
{
  if (!v || !origin || !key) return;
  web_storage_kv **pp = &v->storage;
  while (*pp) {
    web_storage_kv *kv = *pp;
    if (strcmp(kv->origin, origin) == 0 && strcmp(kv->key, key) == 0) {
      *pp = kv->next;
      ccord_free(kv->origin); ccord_free(kv->key); ccord_free(kv->val);
      ccord_free(kv);
      return;
    }
    pp = &(*pp)->next;
  }
}

// ---- Frame lifecycle ----

web_frame *web_frame_create(ccord_web_view *v, web_frame *parent, web_node *host_iframe,
                            const char *url, float w, float h, bool is_worker)
{
  if (!v || v->nframes >= WEB_MAX_FRAMES) return nullptr;
  web_frame *f = ccord_calloc(1, sizeof *f);
  if (!f) return nullptr;
  f->id = ++v->next_frame_id;
  f->view = v;
  f->parent = parent;
  f->host_iframe = host_iframe;
  f->is_root = (parent == nullptr && !is_worker);
  f->is_worker = is_worker;
  f->viewport_w = w > 1.0f ? w : 420.0f;
  f->viewport_h = h > 1.0f ? h : 540.0f;
  f->win_obj = JS_UNDEFINED;
  f->doc_obj = JS_UNDEFINED;
  f->worker_host_obj = JS_UNDEFINED;
  if (!web_url_parse(url ? url : "about:blank", parent ? &parent->url : nullptr, &f->url)) {
    (void)web_url_parse("about:blank", nullptr, &f->url);
  }
  f->ctx = JS_NewContext(v->rt);
  if (!f->ctx) { ccord_free(f); return nullptr; }
  JS_SetContextOpaque(f->ctx, f);
  v->frames[v->nframes++] = f;

  if (!is_worker) web_dom_init_frame(f);
  web_js_init_frame(f);
  return f;
}

void web_frame_destroy(web_frame *f)
{
  if (!f || !f->view) return;
  ccord_web_view *v = f->view;

  // Destroy child frames owned by this frame first
  for (size_t i = 0; i < v->nframes; ) {
    web_frame *other = v->frames[i];
    if (other && other != f && (other->parent == f || other->worker_owner == f)) {
      web_frame_destroy(other);
      i = 0; // restart since array shifted
    } else {
      i++;
    }
  }

  web_net_cancel_frame(v, f);
  cancel_timers_for_frame(v, f);

  // Drop queued postMessages targeting or sourced from this frame
  web_postmsg **mp = &v->msg_head;
  v->msg_tail = nullptr;
  while (*mp) {
    web_postmsg *m = *mp;
    if (m->target == f || m->source == f) {
      *mp = m->next;
      if (v->msg_count > 0) v->msg_count--;
      ccord_free(m->origin); ccord_free(m->json_payload); ccord_free(m);
    } else {
      v->msg_tail = m;
      mp = &m->next;
    }
  }

  // Drop MutationObservers for this frame
  web_mut_obs **op = &v->mut_obs;
  while (*op) {
    web_mut_obs *mo = *op;
    if (mo->frame == f) {
      *op = mo->next;
      if (f->ctx) JS_FreeValue(f->ctx, mo->cb);
      ccord_free(mo);
    } else {
      op = &mo->next;
    }
  }

  if (v->captured_frame == f) { v->captured_frame = nullptr; v->captured_node = nullptr; }
  if (f->host_iframe && f->host_iframe->child_frame == f) {
    f->host_iframe->child_frame = nullptr;
  }

  // Unlink from v->frames
  for (size_t i = 0; i < v->nframes; i++) {
    if (v->frames[i] == f) {
      for (size_t j = i + 1u; j < v->nframes; j++) v->frames[j - 1u] = v->frames[j];
      v->frames[--v->nframes] = nullptr;
      break;
    }
  }

  if (f->worker_owner && f->worker_owner->ctx && !JS_IsUndefined(f->worker_host_obj)) {
    JS_FreeValue(f->worker_owner->ctx, f->worker_host_obj);
    f->worker_host_obj = JS_UNDEFINED;
  }

  web_css_sheet_free(&f->sheet);
  web_node_free_all_in_frame(f);
  if (f->ctx) {
    if (!JS_IsUndefined(f->doc_obj)) JS_FreeValue(f->ctx, f->doc_obj);
    if (!JS_IsUndefined(f->win_obj)) JS_FreeValue(f->ctx, f->win_obj);
    JS_FreeContext(f->ctx);
    f->ctx = nullptr;
  }
  ccord_free(f);
}

void web_drain_jobs(ccord_web_view *v)
{
  if (!v || !v->rt || v->destroying) return;
  JSContext *jctx = nullptr;
  int guard = 0;
  while (guard++ < 512) {
    int err = JS_ExecutePendingJob(v->rt, &jctx);
    if (err == 0) break;
    if (err < 0 && jctx) {
      web_frame *f = JS_GetContextOpaque(jctx);
      if (f) web_js_log_exception(f, "microtask");
    }
  }
}

// ---- Public API ----

ccord_web_view *ccord_web_view_create(const ccord_web_config *cfg, ccord_error *err)
{
  ccord_error_clear(err);
  ccord_web_view *v = ccord_calloc(1, sizeof *v);
  if (!v) {
    ccord_error_set(err, CCORD_ERR_NOMEM, "web: out of memory");
    return nullptr;
  }
  if (cfg) v->cfg = *cfg;
  if (v->cfg.viewport_w <= 0.0f) v->cfg.viewport_w = 420.0f;
  if (v->cfg.viewport_h <= 0.0f) v->cfg.viewport_h = 540.0f;
  snprintf(v->user_agent, sizeof v->user_agent, "%s",
           (v->cfg.user_agent && v->cfg.user_agent[0]) ? v->cfg.user_agent : WEB_DEFAULT_UA);
  v->start_ms = ccord_clock_ms();
  v->now_ms = v->start_ms;
  v->dirty = true;

  v->rt = JS_NewRuntime();
  if (!v->rt) {
    ccord_free(v);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "web: QuickJS runtime init failed");
    return nullptr;
  }
  JS_SetRuntimeOpaque(v->rt, v);
  JS_SetMemoryLimit(v->rt, 256u * 1024u * 1024u);
  JS_SetMaxStackSize(v->rt, 2u * 1024u * 1024u);
  JS_SetHostPromiseRejectionTracker(v->rt, host_promise_rejection_tracker, v);

  web_dom_register_classes(v);
  web_canvas_register_classes(v);
  web_js_register_classes(v);
  web_wasm_register_classes(v);

  if (!web_net_init(v)) {
    JS_FreeRuntime(v->rt);
    ccord_free(v);
    ccord_error_set(err, CCORD_ERR_INTERNAL, "web: curl_multi init failed");
    return nullptr;
  }
  return v;
}

void ccord_web_view_destroy(ccord_web_view *v)
{
  if (!v) return;
  v->destroying = true;
  web_net_shutdown(v);
  while (v->nframes > 0) web_frame_destroy(v->frames[v->nframes - 1u]);
  v->root = nullptr;
  for (size_t i = 0; i < v->nblobs; i++) ccord_free(v->blobs[i].data);
  web_storage_kv *kv = v->storage;
  while (kv) {
    web_storage_kv *next = kv->next;
    ccord_free(kv->origin); ccord_free(kv->key); ccord_free(kv->val);
    ccord_free(kv);
    kv = next;
  }
  if (v->rt) JS_FreeRuntime(v->rt);
  ccord_free(v);
}

ccord_status ccord_web_view_load_html(ccord_web_view *v, const char *html, size_t len,
                                      const char *base_url, ccord_error *err)
{
  ccord_error_clear(err);
  if (!v || !html) return ccord_error_set(err, CCORD_ERR_INVALID, "web: null argument");
  if (v->root) {
    web_frame_destroy(v->root);
    v->root = nullptr;
  }
  v->root = web_frame_create(v, nullptr, nullptr, base_url ? base_url : "https://discord.com/",
                             v->cfg.viewport_w, v->cfg.viewport_h, false);
  if (!v->root) return ccord_error_set(err, CCORD_ERR_NOMEM, "web: frame init failed");
  if (!web_dom_parse_document(v->root, html, len)) {
    return ccord_error_set(err, CCORD_ERR_PARSE, "web: HTML parse failed");
  }
  web_dom_run_scripts(v->root);
  web_drain_jobs(v);
  v->dirty = true;
  return CCORD_OK;
}

ccord_status ccord_web_view_load_url(ccord_web_view *v, const char *url, ccord_error *err)
{
  ccord_error_clear(err);
  if (!v || !url || !url[0]) return ccord_error_set(err, CCORD_ERR_INVALID, "web: empty URL");
  if (!web_net_url_allowed(v, url)) return ccord_error_set(err, CCORD_ERR_PERMISSION, "web: URL blocked by policy");
  if (!v->root) {
    v->root = web_frame_create(v, nullptr, nullptr, url, v->cfg.viewport_w, v->cfg.viewport_h, false);
    if (!v->root) return ccord_error_set(err, CCORD_ERR_NOMEM, "web: frame init failed");
  }
  if (!web_net_start(v, v->root, WEB_FETCH_TOP_DOC, url, "GET", nullptr, 0, nullptr, 0)) {
    return ccord_error_set(err, CCORD_ERR_IO, "web: failed to start fetch");
  }
  return CCORD_OK;
}

static bool safe_sitekey(const char *s)
{
  if (!s || !s[0] || strlen(s) > 120u) return false;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (!(isalnum(*p) || *p == '-' || *p == '_')) return false;
  }
  return true;
}

static bool safe_host(const char *s)
{
  if (!s || !s[0] || strlen(s) > 120u) return false;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (!(isalnum(*p) || *p == '-' || *p == '.')) return false;
  }
  return true;
}

ccord_status ccord_web_view_load_captcha(ccord_web_view *v, const char *service,
                                         const char *sitekey, const char *rqdata,
                                         bool dark_theme, ccord_error *err)
{
  ccord_error_clear(err);
  if (!v || !safe_sitekey(sitekey)) {
    return ccord_error_set(err, CCORD_ERR_INVALID, "web: invalid captcha sitekey");
  }
  if (service && service[0] && strcmp(service, "hcaptcha") != 0) {
    return ccord_error_set(err, CCORD_ERR_UNSUPPORTED, "web: unsupported captcha service '%s'", service);
  }
  const char *host = (v->cfg.captcha_host && safe_host(v->cfg.captcha_host)) ? v->cfg.captcha_host : "discord.com";
  // JSON-encode sitekey and rqdata safely so arbitrary characters can never break out of JS string literals.
  ccord_buf sk_js, rq_js;
  ccord_buf_init(&sk_js, 512);
  ccord_buf_init(&rq_js, 16384);
  ccord_jw w1, w2;
  ccord_jw_init(&w1, &sk_js); ccord_jw_cstr(&w1, sitekey);
  ccord_jw_init(&w2, &rq_js); ccord_jw_cstr(&w2, rqdata ? rqdata : "");

  ccord_buf html;
  ccord_buf_init(&html, 32768);
  char bg[16], fg[16];
  if (v->cfg.page_bg.a > 0) {
    snprintf(bg, sizeof bg, "#%02x%02x%02x", v->cfg.page_bg.r, v->cfg.page_bg.g, v->cfg.page_bg.b);
  } else {
    snprintf(bg, sizeof bg, "%s", dark_theme ? "#1e1f22" : "#ffffff");
  }
  if (v->cfg.page_fg.a > 0) {
    snprintf(fg, sizeof fg, "#%02x%02x%02x", v->cfg.page_fg.r, v->cfg.page_fg.g, v->cfg.page_fg.b);
  } else {
    snprintf(fg, sizeof fg, "%s", dark_theme ? "#dbdee1" : "#1e1f22");
  }
  const char *theme = dark_theme ? "dark" : "light";

  char part1[1024];
  snprintf(part1, sizeof part1,
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<style>"
    "html,body{margin:0;padding:0;width:100%%;height:100%%;background:%s;color:%s;font-family:sans-serif;overflow:hidden;}"
    "body{display:flex;flex-direction:column;align-items:center;justify-content:center;}"
    "#captcha-box{min-height:78px;display:flex;justify-content:center;align-items:center;}"
    "</style></head><body>"
    "<div id=\"captcha-box\"></div>"
    "<script>"
    "const __SITEKEY = ", bg, fg);
  (void)ccord_buf_append_str(&html, part1);
  (void)ccord_buf_append_str(&html, ccord_buf_cstr(&sk_js));
  (void)ccord_buf_append_str(&html, ";\nconst __RQDATA = ");
  (void)ccord_buf_append_str(&html, ccord_buf_cstr(&rq_js));

  char part2[2048];
  snprintf(part2, sizeof part2,
    ";\nlet __rendered = false;"
    "window.__ccordHcaptchaOnload = function(){"
    "  if (__rendered) return;"
    "  if (typeof hcaptcha === 'undefined') { ccord.postMessage('error','hcaptcha unavailable'); return; }"
    "  __rendered = true;"
    "  try {"
    "    const wid = hcaptcha.render('captcha-box', {"
    "      sitekey: __SITEKEY, theme: '%s',"
    "      callback: function(tok){ ccord.postMessage('solved', tok); },"
    "      'expired-callback': function(){ ccord.postMessage('expired', ''); },"
    "      'error-callback': function(e){ ccord.postMessage('error', String(e)); }"
    "    });"
    "    if (__RQDATA && typeof hcaptcha.setData === 'function') hcaptcha.setData(wid, { rqdata: __RQDATA });"
    "    ccord.postMessage('ready', String(wid));"
    "  } catch(e) {"
    "    ccord.postMessage('error', String(e));"
    "  }"
    "};"
    "window.addEventListener('error', function(e){ console.error('window error:', e.message || e); });"
    "</script>"
    "<script id=\"hc-api\" src=\"https://js.hcaptcha.com/1/api.js?render=explicit&host=%s&onload=__ccordHcaptchaOnload&recaptchacompat=off\" async defer></script>"
    "<script>"
    "document.getElementById('hc-api').addEventListener('load', function(){"
    "  setTimeout(function(){ if (!__rendered && typeof window.hcaptcha !== 'undefined') window.__ccordHcaptchaOnload(); }, 100);"
    "});"
    "</script>"
    "</body></html>", theme, host);
  (void)ccord_buf_append_str(&html, part2);

  ccord_buf_free(&sk_js);
  ccord_buf_free(&rq_js);
  char base_url[192];
  snprintf(base_url, sizeof base_url, "https://%s/login", host);
  ccord_status st = ccord_web_view_load_html(v, html.data, html.len, base_url, err);
  ccord_buf_free(&html);
  return st;
}

void ccord_web_view_pump(ccord_web_view *v, uint64_t now_ms)
{
  if (!v || v->destroying) return;
  if (v->rt) JS_UpdateStackTop(v->rt);
  v->now_ms = now_ms ? now_ms : ccord_clock_ms();

  // 1. Pump non-blocking HTTP fetches
  web_net_pump(v);

  // 2. Deliver queued cross-frame postMessage events
  int msg_budget = 64;
  while (v->msg_head && msg_budget-- > 0) {
    web_postmsg *m = v->msg_head;
    v->msg_head = m->next;
    if (!v->msg_head) v->msg_tail = nullptr;
    if (v->msg_count > 0) v->msg_count--;
    web_js_deliver_postmsg(v, m);
    ccord_free(m->origin);
    ccord_free(m->json_payload);
    ccord_free(m);
    web_drain_jobs(v);
  }

  // 3. Fire due timers & requestAnimationFrame callbacks
  for (size_t i = 0; i < WEB_MAX_TIMERS; i++) {
    web_timer *t = &v->timers[i];
    if (!t->active || t->due_ms > v->now_ms) continue;
    web_frame *f = t->frame;
    if (!f || !f->ctx) { web_cancel_timer(v, t->id); continue; }
    uint32_t fid = f->id;
    JSValue fn = JS_IsUndefined(t->fn) ? JS_UNDEFINED : JS_DupValue(f->ctx, t->fn);
    char *code = t->code_str ? ccord_strdup(t->code_str) : nullptr;
    bool is_raf = t->is_raf;
    if (t->is_interval) {
      t->due_ms = v->now_ms + (t->interval_ms ? t->interval_ms : 16u);
    } else {
      web_cancel_timer(v, t->id);
    }
    if (!JS_IsUndefined(fn)) {
      JSValue arg = JS_NewFloat64(f->ctx, (double)(v->now_ms - v->start_ms));
      JSValue r = JS_Call(f->ctx, fn, f->win_obj, is_raf ? 1 : 0, &arg);
      if (web_frame_alive(v, fid)) {
        if (JS_IsException(r)) web_js_log_exception(f, is_raf ? "rAF" : "timer");
        JS_FreeValue(f->ctx, r);
        JS_FreeValue(f->ctx, fn);
      }
    } else if (code) {
      if (strstr(code, "resize") != nullptr) f->resize_pending = false;
      web_dom_exec_script_code(f, nullptr, code, strlen(code), "<timer>");
    }
    ccord_free(code);
    web_drain_jobs(v);
  }

  // 4. Drain any remaining microtasks
  web_drain_jobs(v);
}

void ccord_web_view_set_viewport(ccord_web_view *v, float w, float h)
{
  if (!v || w <= 1.0f || h <= 1.0f) return;
  if (fabsf(v->cfg.viewport_w - w) < 0.5f && fabsf(v->cfg.viewport_h - h) < 0.5f) return;
  v->cfg.viewport_w = w;
  v->cfg.viewport_h = h;
  if (v->root) {
    v->root->viewport_w = w;
    v->root->viewport_h = h;
    v->root->style_dirty = true;
    v->root->layout_dirty = true;
    if (v->root->ctx && !v->destroying) {
      if (v->rt) JS_UpdateStackTop(v->rt);
      JS_SetPropertyStr(v->root->ctx, v->root->win_obj, "innerWidth", JS_NewFloat64(v->root->ctx, (double)w));
      JS_SetPropertyStr(v->root->ctx, v->root->win_obj, "innerHeight", JS_NewFloat64(v->root->ctx, (double)h));
      JS_SetPropertyStr(v->root->ctx, v->root->win_obj, "outerWidth", JS_NewFloat64(v->root->ctx, (double)w));
      JS_SetPropertyStr(v->root->ctx, v->root->win_obj, "outerHeight", JS_NewFloat64(v->root->ctx, (double)h));
      JSValue rev = web_js_make_event(v->root->ctx, "resize", false, false);
      web_dom_dispatch_win_event(v->root, rev);
      JS_FreeValue(v->root->ctx, rev);
      web_drain_jobs(v);
    }
  }
  v->dirty = true;
}

bool ccord_web_view_needs_redraw(const ccord_web_view *v)
{
  return v && v->dirty;
}

uint32_t ccord_web_view_next_wait_ms(const ccord_web_view *v, uint64_t now_ms)
{
  if (!v) return UINT32_MAX;
  if (v->msg_head != nullptr) return 0u;
  uint32_t best = v->nfetches > 0 ? 16u : UINT32_MAX;
  for (size_t i = 0; i < WEB_MAX_TIMERS; i++) {
    if (!v->timers[i].active) continue;
    uint32_t rem = v->timers[i].due_ms <= now_ms ? 0u : (uint32_t)(v->timers[i].due_ms - now_ms);
    if (rem < best) best = rem;
  }
  return best;
}

void ccord_web_view_draw(ccord_web_view *v, ccord_renderer *r, ccord_rect bounds)
{
  if (!v || !r || bounds.w <= 1.0f || bounds.h <= 1.0f) return;
  v->measure_r = r;
  ccord_web_view_set_viewport(v, bounds.w, bounds.h);
  if (v->root) {
    web_frame_paint(v->root, r, bounds);
  }
  v->dirty = false;
}

bool ccord_web_view_handle_input(ccord_web_view *v, ccord_rect bounds, const ccord_input_event *ev)
{
  return web_view_dispatch_input(v, bounds, ev);
}

bool ccord_web_view_eval_debug(ccord_web_view *v, const char *js, char *out, size_t cap)
{
  if (out && cap > 0) out[0] = '\0';
  if (!v || !v->root || !v->root->ctx || !js) return false;
  JS_UpdateStackTop(v->rt);
  JSValue res = JS_Eval(v->root->ctx, js, strlen(js), "<eval>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(res)) {
    web_js_log_exception(v->root, "<eval>");
    return false;
  }
  if (out && cap > 0) {
    const char *s = JS_ToCString(v->root->ctx, res);
    if (s) {
      snprintf(out, cap, "%s", s);
      JS_FreeCString(v->root->ctx, s);
    } else {
      JSValue ex = JS_GetException(v->root->ctx);
      JS_FreeValue(v->root->ctx, ex);
    }
  }
  JS_FreeValue(v->root->ctx, res);
  web_drain_jobs(v);
  return true;
}

const char *ccord_web_view_diagnostics(const ccord_web_view *v)
{
  return v ? v->diag : "";
}

size_t ccord_web_view_pending_fetches(const ccord_web_view *v)
{
  return v ? v->nfetches : 0u;
}

static bool node_chain_visible(const web_node *n)
{
  for (const web_node *cur = n; cur; cur = cur->parent) {
    if (cur->type != WEB_NODE_ELEMENT) continue;
    const web_style *st = &cur->computed;
    if (st->display == WEB_DISP_NONE || st->hidden || st->opacity <= 0.05f ||
        fabsf(st->scale_x) <= 0.05f || fabsf(st->scale_y) <= 0.05f) {
      return false;
    }
  }
  return true;
}

void ccord_web_view_captcha_state(const ccord_web_view *v, ccord_web_captcha_state *out)
{
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!v || !v->root) return;
  if (v->measure_r && (v->root->style_dirty || v->root->layout_dirty)) {
    web_frame_ensure_layout(v->root, v->measure_r);
  }
  for (size_t i = 0; i < v->nframes; i++) {
    const web_frame *f = v->frames[i];
    if (!f || f->is_root || f->is_worker || !f->host_iframe) continue;
    const web_node *hn = f->host_iframe;
    if (strstr(f->url.href, "frame=checkbox") != nullptr) {
      if (f->all_nodes_len > 20u && hn->box_valid && hn->box.w > 50.0f && hn->box.h > 30.0f) {
        out->checkbox_ready = true;
      }
    } else if (strstr(f->url.href, "frame=challenge") != nullptr) {
      if (node_chain_visible(hn) && hn->box_valid && hn->box.w > 20.0f && hn->box.h > 20.0f) {
        out->challenge_open = true;
        out->challenge_w = hn->box.w;
        out->challenge_h = hn->box.h;
        if (hn->parent && hn->parent->box_valid && hn->parent->box.h > out->challenge_h &&
            hn->parent->box.h <= 700.0f) {
          out->challenge_h = hn->parent->box.h;
        }
      }
    }
  }
}
