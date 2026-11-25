// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "web_internal.h"

#include <ccord/core/base64.h>
#include <ccord/core/os.h>
#include <ccord/core/random.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Exception & Console logging ----

// Compacts long URL query/hash fragments in JS stack traces while preserving function names and :line:col.
void web_js_format_stack(const char *raw, char *out, size_t cap)
{
  if (!out || cap == 0) return;
  out[0] = '\0';
  if (!raw || !raw[0]) return;
  size_t w = 0;
  for (const char *p = raw; *p && w + 4u < cap; ) {
    if (*p == '\n' || *p == '\r') {
      if (w > 0 && out[w - 1u] != ' ') {
        if (w + 3u < cap) { out[w++] = ' '; out[w++] = '|'; out[w++] = ' '; }
      }
      while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') p++;
      continue;
    }
    if (*p == '#' && strncmp(p, "#frame=", 7) == 0) {
      // Keep "#frame=<kind>" and skip the remaining hash params up to ":line:col)" or ")"
      const char *amp = strchr(p, '&');
      const char *rp = strchr(p, ')');
      const char *nl = strchr(p, '\n');
      const char *end = rp && (!nl || rp < nl) ? rp : nl;
      if (amp && (!end || amp < end)) {
        size_t keep = (size_t)(amp - p);
        if (w + keep < cap - 2u) { memcpy(out + w, p, keep); w += keep; }
        // Preserve trailing :line:col before ')'
        const char *colon = end ? end : (p + strlen(p));
        const char *scan = colon;
        int colons = 0;
        while (scan > amp && colons < 2) {
          scan--;
          if (*scan == ':') colons++;
          else if (!isdigit((unsigned char)*scan)) break;
        }
        if (colons >= 1 && *scan == ':') p = scan;
        else p = colon;
        continue;
      }
    }
    out[w++] = *p++;
  }
  out[w] = '\0';
}

static uint32_t fnv1a_str(const char *s)
{
  uint32_t h = 2166136261u;
  if (!s) return h;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    h ^= (uint32_t)*p;
    h *= 16777619u;
  }
  return h;
}

void web_js_log_exception(web_frame *f, const char *where)
{
  if (!f || !f->ctx) return;
  JSValue ex = JS_GetException(f->ctx);
  const char *msg = JS_ToCString(f->ctx, ex);
  if (!msg) { JSValue e2 = JS_GetException(f->ctx); JS_FreeValue(f->ctx, e2); }
  JSValue st = JS_IsObject(ex) ? JS_GetPropertyStr(f->ctx, ex, "stack") : JS_UNDEFINED;
  const char *stack = JS_IsString(st) ? JS_ToCString(f->ctx, st) : nullptr;
  if (JS_IsString(st) && !stack) { JSValue e2 = JS_GetException(f->ctx); JS_FreeValue(f->ctx, e2); }
  char fstack[420];
  web_js_format_stack(stack, fstack, sizeof fstack);

  bool suppress = false;
  if (where && strcmp(where, "rAF") == 0) {
    uint32_t h = fnv1a_str(msg ? msg : "") ^ (fnv1a_str(fstack) * 31u);
    if (h == f->last_raf_err_hash) {
      f->last_raf_err_count++;
      if (f->last_raf_err_count > 3u && (f->last_raf_err_count % 120u) != 0u) suppress = true;
    } else {
      f->last_raf_err_hash = h;
      f->last_raf_err_count = 1u;
    }
  }
  if (!suppress) {
    web_diag_append(f->view, "js-err", "[%s] %s%s%s", where ? where : "js",
                    msg ? msg : "(exception)", fstack[0] ? " | " : "", fstack);
    CCORD_LOG_W("web[%u] %s: %s%s%s", f->id, where ? where : "js",
                msg ? msg : "(exception)", fstack[0] ? " | " : "", fstack);
  }
  if (msg) JS_FreeCString(f->ctx, msg);
  if (stack) JS_FreeCString(f->ctx, stack);
  JS_FreeValue(f->ctx, st);
  JS_FreeValue(f->ctx, ex);
}

static JSValue js_console_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f) return JS_UNDEFINED;
  static const char *const k_lvl[4] = {"log", "warn", "error", "debug"};
  const char *lvl = k_lvl[magic & 3];
  ccord_buf b;
  ccord_buf_init(&b, 4096);
  for (int i = 0; i < argc; i++) {
    if (i > 0) (void)ccord_buf_append_str(&b, " ");
    const char *s = JS_ToCString(ctx, argv[i]);
    if (s) {
      (void)ccord_buf_append_str(&b, s);
      JS_FreeCString(ctx, s);
    } else {
      JSValue ex = JS_GetException(ctx);
      JS_FreeValue(ctx, ex);
      (void)ccord_buf_append_str(&b, "[unprintable]");
    }
  }
  web_diag_append(f->view, lvl, "[f%u] %s", f->id, b.data ? b.data : "");
  ccord_buf_free(&b);
  return JS_UNDEFINED;
}

// ---- Timers & performance ----

static JSValue js_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 1) return JS_NewInt32(ctx, 0);
  uint32_t delay = 0;
  if (magic == 2) {
    delay = 16u;
  } else if (argc > 1) {
    int32_t d = 0;
    if (JS_ToInt32(ctx, &d, argv[1]) == 0 && d > 0) delay = (uint32_t)d;
  }
  uint32_t id = 0;
  if (JS_IsFunction(ctx, argv[0])) {
    id = web_add_timer(f, argv[0], nullptr, delay, magic == 1, magic == 2);
  } else if (JS_IsString(argv[0])) {
    const char *code = JS_ToCString(ctx, argv[0]);
    id = web_add_timer(f, JS_UNDEFINED, code, delay, magic == 1, false);
    if (code) JS_FreeCString(ctx, code);
  }
  return JS_NewUint32(ctx, id);
}

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 1) return JS_UNDEFINED;
  uint32_t id = 0;
  if (JS_ToUint32(ctx, &id, argv[0]) == 0) web_cancel_timer(f->view, id);
  return JS_UNDEFINED;
}

static JSValue js_perf_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val; (void)argc; (void)argv;
  web_frame *f = JS_GetContextOpaque(ctx);
  uint64_t now = ccord_clock_ms();
  uint64_t base = (f && f->view) ? f->view->start_ms : now;
  return JS_NewFloat64(ctx, (double)(now >= base ? now - base : 0u));
}

// ---- Base64 & Crypto (OpenSSL + CSPRNG) ----

static JSValue js_atob(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1) return JS_ThrowTypeError(ctx, "atob requires 1 argument");
  size_t len = 0;
  const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
  if (!s) return JS_EXCEPTION;
  // WHATWG atob strips ASCII whitespace (\t \n \f \r ' ') before decoding.
  char *clean = ccord_malloc(len + 1u);
  if (!clean) { JS_FreeCString(ctx, s); return JS_ThrowOutOfMemory(ctx); }
  size_t clen = 0;
  for (size_t i = 0; i < len; i++) {
    char ch = s[i];
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') continue;
    clean[clen++] = ch;
  }
  clean[clen] = '\0';
  JS_FreeCString(ctx, s);
  size_t dlen = 0;
  uint8_t *dec = ccord_base64_decode_alloc(clean, clen, &dlen);
  ccord_free(clean);
  if (!dec) return JS_ThrowTypeError(ctx, "Invalid base64");
  // Convert bytes to JS Latin-1 string (atob spec: each byte -> U+00xx)
  ccord_buf u8;
  ccord_buf_init(&u8, dlen * 2u + 1u);
  for (size_t i = 0; i < dlen; i++) {
    uint8_t b = dec[i];
    if (b < 0x80u) {
      (void)ccord_buf_append(&u8, &b, 1);
    } else {
      uint8_t seq[2] = {(uint8_t)(0xC0u | (b >> 6)), (uint8_t)(0x80u | (b & 0x3Fu))};
      (void)ccord_buf_append(&u8, seq, 2);
    }
  }
  ccord_free(dec);
  JSValue r = JS_NewStringLen(ctx, u8.data ? u8.data : "", u8.len);
  ccord_buf_free(&u8);
  return r;
}

static JSValue js_btoa(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1) return JS_ThrowTypeError(ctx, "btoa requires 1 argument");
  size_t len = 0;
  const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
  if (!s) return JS_EXCEPTION;
  // Convert UTF-8 codepoints (0..255) back to raw Latin-1 bytes
  uint8_t *raw = ccord_malloc(len + 1u);
  if (!raw) { JS_FreeCString(ctx, s); return JS_ThrowOutOfMemory(ctx); }
  size_t rlen = 0;
  for (size_t i = 0; i < len; ) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80u) { raw[rlen++] = c; i++; }
    else if ((c & 0xE0u) == 0xC0u && i + 1u < len) {
      uint32_t cp = ((uint32_t)(c & 0x1Fu) << 6) | ((uint32_t)s[i + 1u] & 0x3Fu);
      raw[rlen++] = (uint8_t)(cp & 0xFFu);
      i += 2u;
    } else {
      raw[rlen++] = c;
      i++;
    }
  }
  JS_FreeCString(ctx, s);
  size_t blen = 0;
  char *enc = ccord_base64_encode_alloc(raw, rlen, &blen);
  ccord_free(raw);
  if (!enc) return JS_ThrowOutOfMemory(ctx);
  JSValue r = JS_NewStringLen(ctx, enc, blen);
  ccord_free(enc);
  return r;
}

static JSValue js_crypto_random(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1) return JS_ThrowTypeError(ctx, "Missing typed array");
  size_t off = 0, blen = 0, bpe = 0, ab_sz = 0;
  JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
  if (JS_IsException(ab)) return ab;
  uint8_t *p = JS_GetArrayBuffer(ctx, &ab_sz, ab);
  JS_FreeValue(ctx, ab);
  if (p && off + blen <= ab_sz && blen > 0) {
    (void)ccord_random_bytes(p + off, blen);
  }
  return JS_DupValue(ctx, argv[0]);
}

static JSValue js_crypto_uuid(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val; (void)argc; (void)argv;
  uint8_t b[16] = {0};
  (void)ccord_random_bytes(b, sizeof b);
  b[6] = (uint8_t)((b[6] & 0x0Fu) | 0x40u);
  b[8] = (uint8_t)((b[8] & 0x3Fu) | 0x80u);
  char s[40];
  snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return JS_NewString(ctx, s);
}

static bool extract_buf_src(JSContext *ctx, JSValueConst val, const uint8_t **out_p, size_t *out_n)
{
  size_t sz = 0;
  uint8_t *p = JS_GetArrayBuffer(ctx, &sz, val);
  if (p) { *out_p = p; *out_n = sz; return true; }
  size_t off = 0, blen = 0, bpe = 0;
  JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &off, &blen, &bpe);
  if (!JS_IsException(ab)) {
    p = JS_GetArrayBuffer(ctx, &sz, ab);
    JS_FreeValue(ctx, ab);
    if (p && off + blen <= sz) { *out_p = p + off; *out_n = blen; return true; }
  } else {
    JSValue ex = JS_GetException(ctx);
    JS_FreeValue(ctx, ex);
  }
  return false;
}

static const EVP_MD *evp_md_from_name(const char *alg)
{
  if (!alg) return EVP_sha256();
  if (ccord_strncasecmp(alg, "SHA-1", 5) == 0) return EVP_sha1();
  if (ccord_strncasecmp(alg, "SHA-256", 7) == 0) return EVP_sha256();
  if (ccord_strncasecmp(alg, "SHA-384", 7) == 0) return EVP_sha384();
  if (ccord_strncasecmp(alg, "SHA-512", 7) == 0) return EVP_sha512();
  return nullptr;
}

static JSValue js_subtle_digest_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  if (argc < 2) return JS_ThrowTypeError(ctx, "digest requires algorithm and data");
  const char *alg = JS_ToCString(ctx, argv[0]);
  if (!alg) return JS_EXCEPTION;
  const EVP_MD *md = evp_md_from_name(alg);
  JS_FreeCString(ctx, alg);
  if (!md) return JS_ThrowTypeError(ctx, "Unsupported digest algorithm");
  const uint8_t *data = nullptr;
  size_t len = 0;
  if (!extract_buf_src(ctx, argv[1], &data, &len)) return JS_ThrowTypeError(ctx, "Expected BufferSource");
  uint8_t out[EVP_MAX_MD_SIZE];
  unsigned int out_len = 0;
  if (!EVP_Digest(data, len, out, &out_len, md, nullptr)) return JS_ThrowInternalError(ctx, "Digest failed");
  return JS_NewArrayBufferCopy(ctx, out, out_len);
}

static JSValue js_subtle_hmac_sync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  if (argc < 3) return JS_ThrowTypeError(ctx, "hmac requires alg, key, data");
  const char *alg = JS_ToCString(ctx, argv[0]);
  if (!alg) return JS_EXCEPTION;
  const EVP_MD *md = evp_md_from_name(alg);
  JS_FreeCString(ctx, alg);
  const uint8_t *key = nullptr, *data = nullptr;
  size_t klen = 0, dlen = 0;
  if (!md || !extract_buf_src(ctx, argv[1], &key, &klen) || !extract_buf_src(ctx, argv[2], &data, &dlen)) {
    return JS_ThrowTypeError(ctx, "Invalid HMAC args");
  }
  uint8_t out[EVP_MAX_MD_SIZE];
  unsigned int out_len = 0;
  if (!HMAC(md, key, (int)klen, data, dlen, out, &out_len)) return JS_ThrowInternalError(ctx, "HMAC failed");
  return JS_NewArrayBufferCopy(ctx, out, out_len);
}

// ---- URL resolution native helper ----

static JSValue js_parse_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  if (argc < 1) return JS_NULL;
  const char *in = JS_ToCString(ctx, argv[0]);
  if (!in) return JS_EXCEPTION;
  const char *base_s = (argc > 1 && JS_IsString(argv[1])) ? JS_ToCString(ctx, argv[1]) : nullptr;
  web_url *base = base_s ? web_url_alloc_parse(base_s, nullptr) : nullptr;
  web_url *out = web_url_alloc_parse(in, base);
  ccord_free(base);
  JS_FreeCString(ctx, in);
  if (base_s) JS_FreeCString(ctx, base_s);
  if (!out) return JS_NULL;
  JSValue o = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, o, "href", JS_NewString(ctx, out->href));
  JS_SetPropertyStr(ctx, o, "origin", JS_NewString(ctx, out->origin));
  JS_SetPropertyStr(ctx, o, "protocol", JS_NewString(ctx, out->protocol));
  JS_SetPropertyStr(ctx, o, "host", JS_NewString(ctx, out->host));
  JS_SetPropertyStr(ctx, o, "hostname", JS_NewString(ctx, out->hostname));
  JS_SetPropertyStr(ctx, o, "port", JS_NewString(ctx, out->port));
  JS_SetPropertyStr(ctx, o, "pathname", JS_NewString(ctx, out->pathname));
  JS_SetPropertyStr(ctx, o, "search", JS_NewString(ctx, out->search));
  JS_SetPropertyStr(ctx, o, "hash", JS_NewString(ctx, out->hash));
  ccord_free(out);
  return o;
}

// ---- Window events, postMessage, Blob, Storage ----

static JSValue js_win_add_el(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 2 || (!JS_IsFunction(ctx, argv[1]) && !JS_IsObject(argv[1]))) return JS_UNDEFINED;
  const char *type = JS_ToCString(ctx, argv[0]);
  if (!type) return JS_EXCEPTION;
  bool cap = false, once = false;
  if (argc > 2) {
    if (JS_IsBool(argv[2])) cap = JS_ToBool(ctx, argv[2]) > 0;
    else if (JS_IsObject(argv[2])) {
      JSValue cv = JS_GetPropertyStr(ctx, argv[2], "capture");
      JSValue ov = JS_GetPropertyStr(ctx, argv[2], "once");
      cap = JS_ToBool(ctx, cv) > 0;
      once = JS_ToBool(ctx, ov) > 0;
      JS_FreeValue(ctx, cv);
      JS_FreeValue(ctx, ov);
    }
  }
  web_listener *l = ccord_calloc(1, sizeof *l);
  if (l) {
    l->type = ccord_strdup(type);
    l->fn = JS_DupValue(ctx, argv[1]);
    l->capture = cap;
    l->once = once;
    web_listener **pp = &f->win_listeners;
    while (*pp) pp = &(*pp)->next;
    *pp = l;
  }
  JS_FreeCString(ctx, type);
  return JS_UNDEFINED;
}

static JSValue js_win_rem_el(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 2) return JS_UNDEFINED;
  const char *type = JS_ToCString(ctx, argv[0]);
  if (!type) return JS_EXCEPTION;
  bool cap = false;
  if (argc > 2) {
    if (JS_IsBool(argv[2])) cap = JS_ToBool(ctx, argv[2]) > 0;
    else if (JS_IsObject(argv[2])) {
      JSValue cv = JS_GetPropertyStr(ctx, argv[2], "capture");
      cap = JS_ToBool(ctx, cv) > 0;
      JS_FreeValue(ctx, cv);
    }
  }
  web_listener **pp = &f->win_listeners;
  while (*pp) {
    web_listener *l = *pp;
    if (strcmp(l->type, type) == 0 && l->capture == cap &&
        JS_VALUE_GET_PTR(l->fn) == JS_VALUE_GET_PTR(argv[1]) &&
        JS_VALUE_GET_TAG(l->fn) == JS_VALUE_GET_TAG(argv[1])) {
      *pp = l->next;
      ccord_free(l->type);
      JS_FreeValue(ctx, l->fn);
      ccord_free(l);
      break;
    }
    pp = &(*pp)->next;
  }
  JS_FreeCString(ctx, type);
  return JS_UNDEFINED;
}

static JSValue js_frame_alive_native(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || !f->view || argc < 1) return JS_FALSE;
  uint32_t fid = 0;
  (void)JS_ToUint32(ctx, &fid, argv[0]);
  return JS_NewBool(ctx, web_frame_alive(f->view, fid));
}

JSValue web_js_get_frame_proxy(web_frame *viewer, uint32_t target_frame_id)
{
  if (!viewer || !viewer->ctx || target_frame_id == 0) return JS_NULL;
  if (target_frame_id == viewer->id) return JS_DupValue(viewer->ctx, viewer->win_obj);
  JSValue fn = JS_GetPropertyStr(viewer->ctx, viewer->win_obj, "__ccordGetFrameProxy");
  if (!JS_IsFunction(viewer->ctx, fn)) {
    JS_FreeValue(viewer->ctx, fn);
    return JS_NULL;
  }
  JSValue arg = JS_NewUint32(viewer->ctx, target_frame_id);
  JSValue res = JS_Call(viewer->ctx, fn, viewer->win_obj, 1, &arg);
  JS_FreeValue(viewer->ctx, fn);
  if (JS_IsException(res)) {
    web_js_log_exception(viewer, "getFrameProxy");
    return JS_NULL;
  }
  return res;
}

void web_js_sync_location(web_frame *f)
{
  if (!f || !f->ctx || JS_IsUndefined(f->win_obj)) return;
  JSContext *ctx = f->ctx;
  JSValue fn = JS_GetPropertyStr(ctx, f->win_obj, "__ccordSyncLocation");
  if (JS_IsFunction(ctx, fn)) {
    JSValue arg = JS_NewString(ctx, f->url.href);
    JSValue r = JS_Call(ctx, fn, f->win_obj, 1, &arg);
    if (JS_IsException(r)) web_js_log_exception(f, "syncLocation");
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, arg);
  }
  JS_FreeValue(ctx, fn);
}

static JSValue js_win_dispatch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (f && argc > 0) web_dom_dispatch_win_event(f, argv[0]);
  return JS_TRUE;
}

static web_frame *find_frame_by_id(ccord_web_view *v, uint32_t id)
{
  for (size_t i = 0; i < v->nframes; i++) {
    if (v->frames[i] && v->frames[i]->id == id) return v->frames[i];
  }
  return nullptr;
}

static JSValue js_post_msg_native(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *src = JS_GetContextOpaque(ctx);
  if (!src || !src->view || argc < 2) return JS_UNDEFINED;
  uint32_t target_id = 0;
  (void)JS_ToUint32(ctx, &target_id, argv[0]);
  web_frame *dst = (target_id == 0) ? src : find_frame_by_id(src->view, target_id);
  if (!dst) return JS_UNDEFINED;

  if (argc > 2 && JS_IsString(argv[2])) {
    const char *torig = JS_ToCString(ctx, argv[2]);
    bool mismatch = torig && strcmp(torig, "*") != 0 && strcmp(torig, "/") != 0 &&
                    strcmp(torig, dst->url.origin) != 0;
    if (torig) JS_FreeCString(ctx, torig);
    if (mismatch) return JS_UNDEFINED;
  }

  bool is_raw_str = JS_IsString(argv[1]);
  const char *payload = nullptr;
  JSValue jstr = JS_UNDEFINED;
  if (is_raw_str) {
    payload = JS_ToCString(ctx, argv[1]);
  } else {
    jstr = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (!JS_IsException(jstr) && JS_IsString(jstr)) payload = JS_ToCString(ctx, jstr);
    else {
      if (JS_IsException(jstr)) { JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex); }
      payload = nullptr;
    }
  }
  web_enqueue_postmsg(src->view, dst, src, src->url.origin, payload ? payload : "null", is_raw_str, 0);
  if (payload) JS_FreeCString(ctx, payload);
  JS_FreeValue(ctx, jstr);
  return JS_UNDEFINED;
}

void web_js_deliver_postmsg(ccord_web_view *v, web_postmsg *m)
{
  if (!v || !m || !m->target || !m->target->ctx) return;
  web_frame *dst = m->target;
  uint32_t dst_fid = dst->id;
  JSContext *ctx = dst->ctx;
  JSValue data = JS_NULL;
  if (m->is_raw_string) {
    data = JS_NewString(ctx, m->json_payload ? m->json_payload : "");
  } else if (m->json_payload) {
    data = JS_ParseJSON(ctx, m->json_payload, strlen(m->json_payload), "<postMessage>");
    if (JS_IsException(data)) {
      JSValue ex = JS_GetException(ctx);
      JS_FreeValue(ctx, ex);
      data = JS_NewString(ctx, m->json_payload);
    }
  }
  JSValue ev = web_js_make_event(ctx, "message", false, false);
  JS_SetPropertyStr(ctx, ev, "data", data);
  JS_SetPropertyStr(ctx, ev, "origin", JS_NewString(ctx, m->origin ? m->origin : ""));
  JS_SetPropertyStr(ctx, ev, "ports", JS_NewArray(ctx));
  if (m->source) {
    JS_SetPropertyStr(ctx, ev, "source", web_js_get_frame_proxy(dst, m->source->id));
  } else {
    JS_SetPropertyStr(ctx, ev, "source", JS_NULL);
  }
  if (dst->is_worker && m->source == dst->worker_owner) {
    // Message delivered into worker's self.onmessage
    web_dom_dispatch_win_event(dst, ev);
  } else if (m->source && m->source->is_worker && dst == m->source->worker_owner &&
             !JS_IsUndefined(m->source->worker_host_obj)) {
    // Message from worker back to parent's Worker instance
    JSValue onm = JS_GetPropertyStr(ctx, m->source->worker_host_obj, "onmessage");
    if (JS_IsFunction(ctx, onm)) {
      JSValue args[1] = {ev};
      JSValue r = JS_Call(ctx, onm, m->source->worker_host_obj, 1, args);
      if (web_frame_alive(v, dst_fid)) {
        if (JS_IsException(r)) web_js_log_exception(dst, "worker.onmessage");
        JS_FreeValue(ctx, r);
      }
    }
    if (web_frame_alive(v, dst_fid)) JS_FreeValue(ctx, onm);
  } else {
    web_dom_dispatch_win_event(dst, ev);
  }
  if (web_frame_alive(v, dst_fid)) JS_FreeValue(ctx, ev);
}

static JSValue js_ccord_host_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || !f->is_root || !f->view || argc < 1) return JS_UNDEFINED;
  const char *kind = JS_ToCString(ctx, argv[0]);
  if (!kind) return JS_EXCEPTION;
  const char *payload = nullptr;
  if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
    payload = JS_ToCString(ctx, argv[1]);
    if (!payload) { JS_FreeCString(ctx, kind); return JS_EXCEPTION; }
  }
  if (f->view->cfg.on_msg) {
    f->view->cfg.on_msg(f->view->cfg.on_msg_ud, kind, payload ? payload : "");
  }
  JS_FreeCString(ctx, kind);
  if (payload) JS_FreeCString(ctx, payload);
  return JS_UNDEFINED;
}

static JSValue js_create_blob_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 1) return JS_NewString(ctx, "");
  const char *mime = (argc > 1 && JS_IsString(argv[1])) ? JS_ToCString(ctx, argv[1]) : nullptr;
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (extract_buf_src(ctx, argv[0], &bytes, &len)) {
    const char *u = web_blob_register(f->view, bytes, len, mime ? mime : "application/octet-stream");
    if (mime) JS_FreeCString(ctx, mime);
    return JS_NewString(ctx, u ? u : "");
  }
  const char *str = JS_ToCStringLen(ctx, &len, argv[0]);
  if (!str) { if (mime) JS_FreeCString(ctx, mime); return JS_EXCEPTION; }
  const char *u = web_blob_register(f->view, (const uint8_t *)str, len, mime ? mime : "text/plain");
  JS_FreeCString(ctx, str);
  if (mime) JS_FreeCString(ctx, mime);
  return JS_NewString(ctx, u ? u : "");
}

static JSValue js_storage_op(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f) return JS_NULL;
  if (magic == 0 && argc > 0) {
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_EXCEPTION;
    const char *v = web_storage_get(f->view, f->url.origin, k);
    JS_FreeCString(ctx, k);
    return v ? JS_NewString(ctx, v) : JS_NULL;
  }
  if (magic == 1 && argc > 1) {
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_EXCEPTION;
    const char *v = JS_ToCString(ctx, argv[1]);
    if (!v) { JS_FreeCString(ctx, k); return JS_EXCEPTION; }
    web_storage_set(f->view, f->url.origin, k, v);
    JS_FreeCString(ctx, k);
    JS_FreeCString(ctx, v);
  }
  if (magic == 2 && argc > 0) {
    const char *k = JS_ToCString(ctx, argv[0]);
    if (!k) return JS_EXCEPTION;
    web_storage_remove(f->view, f->url.origin, k);
    JS_FreeCString(ctx, k);
  }
  return JS_UNDEFINED;
}

// ---- Async fetch() & XMLHttpRequest ----

static JSValue js_fetch_native(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 1) return JS_ThrowTypeError(ctx, "fetch requires URL");
  const char *url = JS_ToCString(ctx, argv[0]);
  if (!url) return JS_EXCEPTION;
  const char *method = (argc > 1 && JS_IsString(argv[1])) ? JS_ToCString(ctx, argv[1]) : nullptr;

  const char *hdr_kv[64];
  size_t nhdrs = 0;
  char *hdr_alloc[64];
  if (argc > 2 && JS_IsArray(argv[2])) {
    JSValue lenv = JS_GetPropertyStr(ctx, argv[2], "length");
    uint32_t n = 0;
    (void)JS_ToUint32(ctx, &n, lenv);
    JS_FreeValue(ctx, lenv);
    for (uint32_t i = 0; i < n && nhdrs < 64u; i++) {
      JSValue elem = JS_GetPropertyUint32(ctx, argv[2], i);
      const char *s = JS_ToCString(ctx, elem);
      JS_FreeValue(ctx, elem);
      if (!s) {
        JSValue ex = JS_GetException(ctx);
        JS_FreeValue(ctx, ex);
      }
      hdr_alloc[nhdrs] = s ? ccord_strdup(s) : ccord_strdup("");
      hdr_kv[nhdrs] = hdr_alloc[nhdrs];
      if (s) JS_FreeCString(ctx, s);
      nhdrs++;
    }
  }

  const char *body_s = nullptr;
  size_t body_len = 0;
  const uint8_t *body_bin = nullptr;
  if (argc > 3 && !JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
    if (!extract_buf_src(ctx, argv[3], &body_bin, &body_len)) {
      body_s = JS_ToCStringLen(ctx, &body_len, argv[3]);
      if (!body_s) {
        for (size_t i = 0; i < nhdrs; i++) ccord_free(hdr_alloc[i]);
        JS_FreeCString(ctx, url);
        if (method) JS_FreeCString(ctx, method);
        return JS_EXCEPTION;
      }
    }
  }

  JSValue funcs[2];
  JSValue prom = JS_NewPromiseCapability(ctx, funcs);
  web_fetch *ft = web_net_start(f->view, f, WEB_FETCH_JS_FETCH, url, method ? method : "GET",
                                hdr_kv, nhdrs, body_bin ? (const char *)body_bin : body_s, body_len);
  for (size_t i = 0; i < nhdrs; i++) ccord_free(hdr_alloc[i]);
  JS_FreeCString(ctx, url);
  if (method) JS_FreeCString(ctx, method);
  if (body_s) JS_FreeCString(ctx, body_s);

  if (ft) {
    ft->resolve_fn = funcs[0];
    ft->reject_fn = funcs[1];
  } else {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "Network request blocked or failed"));
    JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
  }
  return prom;
}

void web_js_on_fetch_done(web_fetch *ft)
{
  if (!ft || !ft->frame || !ft->frame->ctx) return;
  JSContext *ctx = ft->frame->ctx;
  if (ft->failed) {
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "Failed to fetch"));
    JSValue r = JS_Call(ctx, ft->reject_fn, JS_UNDEFINED, 1, &err);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, err);
    return;
  }
  JSValue payload = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, payload, "status", JS_NewInt32(ctx, (int32_t)ft->http_status));
  JS_SetPropertyStr(ctx, payload, "url", JS_NewString(ctx, ft->final_url));
  JS_SetPropertyStr(ctx, payload, "headersRaw",
                    JS_NewStringLen(ctx, ft->resp_headers.data ? ft->resp_headers.data : "", ft->resp_headers.len));
  JS_SetPropertyStr(ctx, payload, "bodyBuf",
                    JS_NewArrayBufferCopy(ctx, (const uint8_t *)(ft->resp_body.data ? ft->resp_body.data : ""),
                                          ft->resp_body.len));
  JS_SetPropertyStr(ctx, payload, "bodyText",
                    JS_NewStringLen(ctx, ft->resp_body.data ? ft->resp_body.data : "", ft->resp_body.len));
  JSValue r = JS_Call(ctx, ft->resolve_fn, JS_UNDEFINED, 1, &payload);
  if (JS_IsException(r)) web_js_log_exception(ft->frame, "fetch resolve");
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, payload);
}

static JSValue js_xhr_send_native(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 4) return JS_UNDEFINED;
  JSValueConst xhr_obj = argv[0];
  const char *method = JS_ToCString(ctx, argv[1]);
  if (!method) return JS_EXCEPTION;
  const char *url = JS_ToCString(ctx, argv[2]);
  if (!url) { JS_FreeCString(ctx, method); return JS_EXCEPTION; }

  const char *hdr_kv[64];
  char *hdr_alloc[64];
  size_t nhdrs = 0;
  if (JS_IsArray(argv[3])) {
    JSValue lenv = JS_GetPropertyStr(ctx, argv[3], "length");
    uint32_t n = 0;
    (void)JS_ToUint32(ctx, &n, lenv);
    JS_FreeValue(ctx, lenv);
    for (uint32_t i = 0; i < n && nhdrs < 64u; i++) {
      JSValue el = JS_GetPropertyUint32(ctx, argv[3], i);
      const char *s = JS_ToCString(ctx, el);
      JS_FreeValue(ctx, el);
      if (!s) {
        JSValue ex = JS_GetException(ctx);
        JS_FreeValue(ctx, ex);
      }
      hdr_alloc[nhdrs] = s ? ccord_strdup(s) : ccord_strdup("");
      hdr_kv[nhdrs] = hdr_alloc[nhdrs];
      if (s) JS_FreeCString(ctx, s);
      nhdrs++;
    }
  }
  const char *body_s = nullptr;
  size_t body_len = 0;
  const uint8_t *body_bin = nullptr;
  if (argc > 4 && !JS_IsNull(argv[4]) && !JS_IsUndefined(argv[4])) {
    if (!extract_buf_src(ctx, argv[4], &body_bin, &body_len)) {
      body_s = JS_ToCStringLen(ctx, &body_len, argv[4]);
      if (!body_s) {
        for (size_t i = 0; i < nhdrs; i++) ccord_free(hdr_alloc[i]);
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
      }
    }
  }
  web_fetch *ft = web_net_start(f->view, f, WEB_FETCH_XHR, url, method,
                                hdr_kv, nhdrs, body_bin ? (const char *)body_bin : body_s, body_len);
  for (size_t i = 0; i < nhdrs; i++) ccord_free(hdr_alloc[i]);
  JS_FreeCString(ctx, method);
  JS_FreeCString(ctx, url);
  if (body_s) JS_FreeCString(ctx, body_s);
  if (ft) {
    ft->xhr_obj = JS_DupValue(ctx, xhr_obj);
  } else {
    JSValue cb = JS_GetPropertyStr(ctx, xhr_obj, "__complete");
    if (JS_IsFunction(ctx, cb)) {
      JSValue args[4] = {JS_FALSE, JS_NewInt32(ctx, 0), JS_NewString(ctx, ""), JS_NewString(ctx, "")};
      JSValue r = JS_Call(ctx, cb, xhr_obj, 4, args);
      JS_FreeValue(ctx, r);
      for (int i = 0; i < 4; i++) JS_FreeValue(ctx, args[i]);
    }
    JS_FreeValue(ctx, cb);
  }
  return JS_UNDEFINED;
}

void web_js_on_xhr_done(web_fetch *ft)
{
  if (!ft || !ft->frame || !ft->frame->ctx || JS_IsUndefined(ft->xhr_obj)) return;
  JSContext *ctx = ft->frame->ctx;
  JSValue cb = JS_GetPropertyStr(ctx, ft->xhr_obj, "__complete");
  if (JS_IsFunction(ctx, cb)) {
    JSValue args[6] = {
      JS_NewBool(ctx, !ft->failed),
      JS_NewInt32(ctx, (int32_t)ft->http_status),
      JS_NewStringLen(ctx, ft->resp_headers.data ? ft->resp_headers.data : "", ft->resp_headers.len),
      JS_NewStringLen(ctx, ft->resp_body.data ? ft->resp_body.data : "", ft->resp_body.len),
      JS_NewArrayBufferCopy(ctx, (const uint8_t *)(ft->resp_body.data ? ft->resp_body.data : ""), ft->resp_body.len),
      JS_NewString(ctx, ft->final_url),
    };
    JSValue r = JS_Call(ctx, cb, ft->xhr_obj, 6, args);
    if (JS_IsException(r)) web_js_log_exception(ft->frame, "xhr complete");
    JS_FreeValue(ctx, r);
    for (int i = 0; i < 6; i++) JS_FreeValue(ctx, args[i]);
  }
  JS_FreeValue(ctx, cb);
}

// ---- Worker ----

static JSValue js_worker_spawn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *parent = JS_GetContextOpaque(ctx);
  if (!parent || !parent->view || argc < 2) return JS_NewUint32(ctx, 0);
  const char *url = JS_ToCString(ctx, argv[0]);
  if (!url) return JS_EXCEPTION;
  web_url *u = web_url_alloc_parse(url, &parent->url);
  JS_FreeCString(ctx, url);
  if (!u) return JS_NewUint32(ctx, 0);

  web_frame *wf = web_frame_create(parent->view, parent, nullptr, u->href, 1, 1, true);
  if (!wf) { ccord_free(u); return JS_NewUint32(ctx, 0); }
  wf->worker_owner = parent;
  wf->worker_host_obj = JS_DupValue(ctx, argv[1]);
  web_fetch *ft = web_net_start(parent->view, wf, WEB_FETCH_WORKER, u->href, "GET", nullptr, 0, nullptr, 0);
  ccord_free(u);
  if (!ft) {
    web_frame_destroy(wf);
    return JS_NewUint32(ctx, 0);
  }
  return JS_NewUint32(ctx, wf->id);
}

void web_js_on_worker_script_done(web_fetch *ft)
{
  if (!ft || !ft->frame || !ft->frame->ctx) return;
  if (!ft->failed && ft->resp_body.data) {
    web_dom_exec_script_code(ft->frame, nullptr, ft->resp_body.data, ft->resp_body.len, ft->final_url);
  }
}

// ---- MutationObserver ----

static JSValue js_mut_observe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  if (!f || argc < 3 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
  web_node *target = web_node_unwrap(ctx, argv[1]);
  if (!target) return JS_UNDEFINED;
  web_mut_obs *mo = ccord_calloc(1, sizeof *mo);
  if (!mo) return JS_UNDEFINED;
  mo->frame = f;
  mo->cb = JS_DupValue(ctx, argv[0]);
  mo->target = target;
  if (JS_IsObject(argv[2])) {
    JSValue cl = JS_GetPropertyStr(ctx, argv[2], "childList");
    JSValue at = JS_GetPropertyStr(ctx, argv[2], "attributes");
    JSValue st = JS_GetPropertyStr(ctx, argv[2], "subtree");
    JSValue cd = JS_GetPropertyStr(ctx, argv[2], "characterData");
    mo->child_list = JS_ToBool(ctx, cl) > 0;
    mo->attributes = JS_ToBool(ctx, at) > 0;
    mo->subtree = JS_ToBool(ctx, st) > 0;
    mo->char_data = JS_ToBool(ctx, cd) > 0;
    JS_FreeValue(ctx, cl); JS_FreeValue(ctx, at); JS_FreeValue(ctx, st); JS_FreeValue(ctx, cd);
  }
  mo->next = f->view->mut_obs;
  f->view->mut_obs = mo;
  return JS_UNDEFINED;
}

// ---- Computed style helper ----

static JSValue js_get_computed_style(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  web_frame *f = JS_GetContextOpaque(ctx);
  web_node *n = (argc > 0) ? web_node_unwrap(ctx, argv[0]) : nullptr;
  if (f) web_frame_ensure_layout(f, f->view->measure_r);
  JSValue o = JS_NewObject(ctx);
  if (n) {
    char buf[64];
    const char *disp = "block";
    if (n->computed.display == WEB_DISP_NONE) disp = "none";
    else if (n->computed.display == WEB_DISP_FLEX) disp = "flex";
    else if (n->computed.display == WEB_DISP_GRID) disp = "grid";
    else if (n->computed.display == WEB_DISP_TABLE) disp = "table";
    else if (n->computed.display == WEB_DISP_TABLE_ROW) disp = "table-row";
    else if (n->computed.display == WEB_DISP_TABLE_CELL) disp = "table-cell";
    else if (n->computed.display == WEB_DISP_INLINE) disp = "inline";
    else if (n->computed.display == WEB_DISP_INLINE_BLOCK) disp = "inline-block";
    JS_SetPropertyStr(ctx, o, "display", JS_NewString(ctx, disp));
    const char *pos = n->computed.position == WEB_POS_ABSOLUTE ? "absolute"
                    : n->computed.position == WEB_POS_FIXED ? "fixed"
                    : n->computed.position == WEB_POS_RELATIVE ? "relative" : "static";
    JS_SetPropertyStr(ctx, o, "position", JS_NewString(ctx, pos));
    JS_SetPropertyStr(ctx, o, "visibility", JS_NewString(ctx, n->computed.hidden ? "hidden" : "visible"));
    JS_SetPropertyStr(ctx, o, "pointerEvents", JS_NewString(ctx, n->computed.pointer_none ? "none" : "auto"));
    JS_SetPropertyStr(ctx, o, "overflow", JS_NewString(ctx, n->computed.overflow_hidden ? "hidden" : "visible"));
    snprintf(buf, sizeof buf, "%d", n->computed.z_index);
    JS_SetPropertyStr(ctx, o, "zIndex", JS_NewString(ctx, n->computed.z_index ? buf : "auto"));
    snprintf(buf, sizeof buf, "%.2f", (double)n->computed.opacity);
    JS_SetPropertyStr(ctx, o, "opacity", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)n->box.w);
    JS_SetPropertyStr(ctx, o, "width", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)n->box.h);
    JS_SetPropertyStr(ctx, o, "height", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)n->computed.font_size);
    JS_SetPropertyStr(ctx, o, "fontSize", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)(n->computed.line_height > 0.0f ? n->computed.line_height : n->computed.font_size * 1.35f));
    JS_SetPropertyStr(ctx, o, "lineHeight", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)n->computed.padding[0]);
    JS_SetPropertyStr(ctx, o, "paddingTop", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)n->computed.padding[3]);
    JS_SetPropertyStr(ctx, o, "paddingLeft", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)n->computed.margin[0]);
    JS_SetPropertyStr(ctx, o, "marginTop", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)n->computed.margin[3]);
    JS_SetPropertyStr(ctx, o, "marginLeft", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%.0fpx", (double)n->computed.border_w[0]);
    JS_SetPropertyStr(ctx, o, "borderTopWidth", JS_NewString(ctx, buf));
    JS_SetPropertyStr(ctx, o, "transform", JS_NewString(ctx, "none"));
    snprintf(buf, sizeof buf, "rgb(%u, %u, %u)", n->computed.color.r, n->computed.color.g, n->computed.color.b);
    JS_SetPropertyStr(ctx, o, "color", JS_NewString(ctx, buf));
    if (n->computed.has_bg) {
      snprintf(buf, sizeof buf, "rgba(%u, %u, %u, %.2f)", n->computed.bg.r, n->computed.bg.g, n->computed.bg.b,
               (double)n->computed.bg.a / 255.0);
      JS_SetPropertyStr(ctx, o, "backgroundColor", JS_NewString(ctx, buf));
    } else {
      JS_SetPropertyStr(ctx, o, "backgroundColor", JS_NewString(ctx, "rgba(0, 0, 0, 0)"));
    }
    static const char k_gcs_fn[] =
      "(function(k){ const c = String(k).replace(/-([a-z])/g, (_,m)=>m.toUpperCase()); return this[c] ?? this[k] ?? ''; })";
    JS_SetPropertyStr(ctx, o, "getPropertyValue",
                      JS_Eval(ctx, k_gcs_fn, sizeof k_gcs_fn - 1u, "<gcs>", JS_EVAL_TYPE_GLOBAL));
  }
  return o;
}

// ---- Event object constructors ----

JSValue web_js_make_event(JSContext *ctx, const char *type, bool bubbles, bool cancelable)
{
  JSValue ev = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, ev, "type", JS_NewString(ctx, type ? type : ""));
  JS_SetPropertyStr(ctx, ev, "bubbles", JS_NewBool(ctx, bubbles));
  JS_SetPropertyStr(ctx, ev, "cancelable", JS_NewBool(ctx, cancelable));
  JS_SetPropertyStr(ctx, ev, "isTrusted", JS_TRUE);
  JS_SetPropertyStr(ctx, ev, "defaultPrevented", JS_FALSE);
  JS_SetPropertyStr(ctx, ev, "_stopProp", JS_FALSE);
  JS_SetPropertyStr(ctx, ev, "_stopImm", JS_FALSE);
  JS_SetPropertyStr(ctx, ev, "timeStamp", JS_NewFloat64(ctx, (double)ccord_clock_ms()));
  static const char k_ev_methods[] =
    "(function(e){"
    "  e.preventDefault = function(){ e.defaultPrevented = true; };"
    "  e.stopPropagation = function(){ e._stopProp = true; };"
    "  e.stopImmediatePropagation = function(){ e._stopProp = true; e._stopImm = true; };"
    "  e.initEvent = function(t,b,c){ e.type=t; e.bubbles=!!b; e.cancelable=!!c; };"
    "})";
  JSValue fn = JS_Eval(ctx, k_ev_methods, sizeof k_ev_methods - 1u, "<ev>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsFunction(ctx, fn)) {
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 1, &ev);
    JS_FreeValue(ctx, r);
  }
  JS_FreeValue(ctx, fn);
  return ev;
}

static JSValue js_touchlist_item(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  if (argc < 1) return JS_NULL;
  int32_t idx = -1;
  if (JS_ToInt32(ctx, &idx, argv[0]) != 0 || idx < 0) return JS_NULL;
  JSValue v = JS_GetPropertyUint32(ctx, this_val, (uint32_t)idx);
  if (JS_IsUndefined(v)) {
    JS_FreeValue(ctx, v);
    return JS_NULL;
  }
  return v;
}

static JSValue make_touch_list(JSContext *ctx, JSValueConst touch_item, bool include)
{
  JSValue arr = JS_NewArray(ctx);
  if (include && !JS_IsUndefined(touch_item)) {
    JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, touch_item));
  }
  JS_SetPropertyStr(ctx, arr, "item", JS_NewCFunction(ctx, js_touchlist_item, "item", 1));
  return arr;
}

JSValue web_js_make_pointer_event_ex(JSContext *ctx, const char *type, float cx, float cy,
                                     float ox, float oy, int button, int buttons,
                                     bool is_pointer, const char *pointer_type, int pointer_id)
{
  JSValue ev = web_js_make_event(ctx, type, true, true);
  JS_SetPropertyStr(ctx, ev, "clientX", JS_NewFloat64(ctx, (double)cx));
  JS_SetPropertyStr(ctx, ev, "clientY", JS_NewFloat64(ctx, (double)cy));
  JS_SetPropertyStr(ctx, ev, "pageX", JS_NewFloat64(ctx, (double)cx));
  JS_SetPropertyStr(ctx, ev, "pageY", JS_NewFloat64(ctx, (double)cy));
  JS_SetPropertyStr(ctx, ev, "screenX", JS_NewFloat64(ctx, (double)cx));
  JS_SetPropertyStr(ctx, ev, "screenY", JS_NewFloat64(ctx, (double)cy));
  JS_SetPropertyStr(ctx, ev, "offsetX", JS_NewFloat64(ctx, (double)ox));
  JS_SetPropertyStr(ctx, ev, "offsetY", JS_NewFloat64(ctx, (double)oy));
  JS_SetPropertyStr(ctx, ev, "x", JS_NewFloat64(ctx, (double)cx));
  JS_SetPropertyStr(ctx, ev, "y", JS_NewFloat64(ctx, (double)cy));
  JS_SetPropertyStr(ctx, ev, "button", JS_NewInt32(ctx, button));
  JS_SetPropertyStr(ctx, ev, "buttons", JS_NewInt32(ctx, buttons));
  JS_SetPropertyStr(ctx, ev, "detail", JS_NewInt32(ctx, 1));
  if (is_pointer) {
    bool is_touch = pointer_type && strcmp(pointer_type, "touch") == 0;
    JS_SetPropertyStr(ctx, ev, "pointerId", JS_NewInt32(ctx, pointer_id));
    JS_SetPropertyStr(ctx, ev, "pointerType", JS_NewString(ctx, pointer_type ? pointer_type : "mouse"));
    JS_SetPropertyStr(ctx, ev, "isPrimary", JS_TRUE);
    JS_SetPropertyStr(ctx, ev, "width", JS_NewFloat64(ctx, is_touch ? 24.0 : 1.0));
    JS_SetPropertyStr(ctx, ev, "height", JS_NewFloat64(ctx, is_touch ? 24.0 : 1.0));
    JS_SetPropertyStr(ctx, ev, "pressure", JS_NewFloat64(ctx, buttons ? (is_touch ? 1.0 : 0.5) : 0.0));
  }
  return ev;
}

JSValue web_js_make_pointer_event(JSContext *ctx, const char *type, float cx, float cy,
                                  float ox, float oy, int button, int buttons, bool is_pointer)
{
  return web_js_make_pointer_event_ex(ctx, type, cx, cy, ox, oy, button, buttons, is_pointer, "mouse", 1);
}

JSValue web_js_make_touch_event(JSContext *ctx, const char *type, JSValueConst target_obj,
                                float cx, float cy, int pointer_id, bool is_active)
{
  bool cancelable = !type || strcmp(type, "touchcancel") != 0;
  JSValue ev = web_js_make_event(ctx, type, true, cancelable);
  JSValue t = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, t, "identifier", JS_NewInt32(ctx, pointer_id));
  JS_SetPropertyStr(ctx, t, "target", JS_IsUndefined(target_obj) ? JS_NULL : JS_DupValue(ctx, target_obj));
  JS_SetPropertyStr(ctx, t, "clientX", JS_NewFloat64(ctx, (double)cx));
  JS_SetPropertyStr(ctx, t, "clientY", JS_NewFloat64(ctx, (double)cy));
  JS_SetPropertyStr(ctx, t, "pageX", JS_NewFloat64(ctx, (double)cx));
  JS_SetPropertyStr(ctx, t, "pageY", JS_NewFloat64(ctx, (double)cy));
  JS_SetPropertyStr(ctx, t, "screenX", JS_NewFloat64(ctx, (double)cx));
  JS_SetPropertyStr(ctx, t, "screenY", JS_NewFloat64(ctx, (double)cy));
  JS_SetPropertyStr(ctx, t, "radiusX", JS_NewFloat64(ctx, 12.0));
  JS_SetPropertyStr(ctx, t, "radiusY", JS_NewFloat64(ctx, 12.0));
  JS_SetPropertyStr(ctx, t, "rotationAngle", JS_NewFloat64(ctx, 0.0));
  JS_SetPropertyStr(ctx, t, "force", JS_NewFloat64(ctx, is_active ? 1.0 : 0.0));
  JS_SetPropertyStr(ctx, ev, "changedTouches", make_touch_list(ctx, t, true));
  JS_SetPropertyStr(ctx, ev, "touches", make_touch_list(ctx, t, is_active));
  JS_SetPropertyStr(ctx, ev, "targetTouches", make_touch_list(ctx, t, is_active));
  JS_FreeValue(ctx, t);
  return ev;
}

JSValue web_js_make_key_event(JSContext *ctx, const char *type, const char *key, const char *code, uint32_t keycode)
{
  JSValue ev = web_js_make_event(ctx, type, true, true);
  JS_SetPropertyStr(ctx, ev, "key", JS_NewString(ctx, key ? key : ""));
  JS_SetPropertyStr(ctx, ev, "code", JS_NewString(ctx, code ? code : ""));
  JS_SetPropertyStr(ctx, ev, "keyCode", JS_NewUint32(ctx, keycode));
  JS_SetPropertyStr(ctx, ev, "which", JS_NewUint32(ctx, keycode));
  return ev;
}

// ---- Frame JS environment bootstrap ----

void web_js_register_classes(ccord_web_view *v)
{
  JS_NewClassID(v->rt, &v->cls_xhr);
  JS_NewClassID(v->rt, &v->cls_port);
  JS_NewClassID(v->rt, &v->cls_mut_obs);
  JS_NewClassID(v->rt, &v->cls_worker);
}

void web_js_init_frame(web_frame *f)
{
  JSContext *ctx = f->ctx;
  JSValue global = JS_GetGlobalObject(ctx);
  f->win_obj = JS_DupValue(ctx, global);

  JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
  JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
  JS_SetPropertyStr(ctx, global, "addEventListener", JS_NewCFunction(ctx, js_win_add_el, "addEventListener", 3));
  JS_SetPropertyStr(ctx, global, "removeEventListener", JS_NewCFunction(ctx, js_win_rem_el, "removeEventListener", 3));
  JS_SetPropertyStr(ctx, global, "dispatchEvent", JS_NewCFunction(ctx, js_win_dispatch, "dispatchEvent", 1));
  JS_SetPropertyStr(ctx, global, "__ccordSetTimeout", JS_NewCFunctionMagic(ctx, js_set_timer, "setTimeout", 2, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, global, "__ccordSetInterval", JS_NewCFunctionMagic(ctx, js_set_timer, "setInterval", 2, JS_CFUNC_generic_magic, 1));
  JS_SetPropertyStr(ctx, global, "requestAnimationFrame", JS_NewCFunctionMagic(ctx, js_set_timer, "requestAnimationFrame", 1, JS_CFUNC_generic_magic, 2));
  JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunction(ctx, js_clear_timer, "clearTimeout", 1));
  JS_SetPropertyStr(ctx, global, "clearInterval", JS_NewCFunction(ctx, js_clear_timer, "clearInterval", 1));
  JS_SetPropertyStr(ctx, global, "cancelAnimationFrame", JS_NewCFunction(ctx, js_clear_timer, "cancelAnimationFrame", 1));
  JS_SetPropertyStr(ctx, global, "atob", JS_NewCFunction(ctx, js_atob, "atob", 1));
  JS_SetPropertyStr(ctx, global, "btoa", JS_NewCFunction(ctx, js_btoa, "btoa", 1));
  JS_SetPropertyStr(ctx, global, "getComputedStyle", JS_NewCFunction(ctx, js_get_computed_style, "getComputedStyle", 1));

  // Internal native bridges used by the JS Web-API shims below
  JS_SetPropertyStr(ctx, global, "__ccordPostMsg", JS_NewCFunction(ctx, js_post_msg_native, "__ccordPostMsg", 3));
  JS_SetPropertyStr(ctx, global, "__ccordFrameAlive", JS_NewCFunction(ctx, js_frame_alive_native, "__ccordFrameAlive", 1));
  JS_SetPropertyStr(ctx, global, "__ccordFetch", JS_NewCFunction(ctx, js_fetch_native, "__ccordFetch", 4));
  JS_SetPropertyStr(ctx, global, "__ccordXhrSend", JS_NewCFunction(ctx, js_xhr_send_native, "__ccordXhrSend", 5));
  JS_SetPropertyStr(ctx, global, "__ccordParseUrl", JS_NewCFunction(ctx, js_parse_url, "__ccordParseUrl", 2));
  JS_SetPropertyStr(ctx, global, "__ccordBlobCreate", JS_NewCFunction(ctx, js_create_blob_url, "__ccordBlobCreate", 2));
  JS_SetPropertyStr(ctx, global, "__ccordWorkerSpawn", JS_NewCFunction(ctx, js_worker_spawn, "__ccordWorkerSpawn", 2));
  JS_SetPropertyStr(ctx, global, "__ccordMutObserve", JS_NewCFunction(ctx, js_mut_observe, "__ccordMutObserve", 3));
  JS_SetPropertyStr(ctx, global, "__ccordDigest", JS_NewCFunction(ctx, js_subtle_digest_sync, "__ccordDigest", 2));
  JS_SetPropertyStr(ctx, global, "__ccordHmac", JS_NewCFunction(ctx, js_subtle_hmac_sync, "__ccordHmac", 3));
  JS_SetPropertyStr(ctx, global, "__ccordStorGet", JS_NewCFunctionMagic(ctx, js_storage_op, "__ccordStorGet", 1, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, global, "__ccordStorSet", JS_NewCFunctionMagic(ctx, js_storage_op, "__ccordStorSet", 2, JS_CFUNC_generic_magic, 1));
  JS_SetPropertyStr(ctx, global, "__ccordStorDel", JS_NewCFunctionMagic(ctx, js_storage_op, "__ccordStorDel", 1, JS_CFUNC_generic_magic, 2));
  JS_SetPropertyStr(ctx, global, "__ccordFrameId", JS_NewUint32(ctx, f->id));
  JS_SetPropertyStr(ctx, global, "__ccordParentId", JS_NewUint32(ctx, f->parent ? f->parent->id : f->id));
  JS_SetPropertyStr(ctx, global, "__ccordTopId", JS_NewUint32(ctx, f->view->root ? f->view->root->id : f->id));
  JS_SetPropertyStr(ctx, global, "__ccordIsRoot", JS_NewBool(ctx, f->is_root));
  JS_SetPropertyStr(ctx, global, "__ccordIsWorker", JS_NewBool(ctx, f->is_worker));
  JS_SetPropertyStr(ctx, global, "__ccordDarkTheme", JS_NewBool(ctx, f->view->cfg.dark_theme));
  JS_SetPropertyStr(ctx, global, "__ccordTouchEnabled", JS_NewBool(ctx, f->view->cfg.touch_enabled));
#if defined(__ANDROID__)
  JS_SetPropertyStr(ctx, global, "__ccordIsAndroid", JS_TRUE);
#else
  JS_SetPropertyStr(ctx, global, "__ccordIsAndroid", JS_FALSE);
#endif
  JS_SetPropertyStr(ctx, global, "__ccordInitialHref", JS_NewString(ctx, f->url.href));
  JS_SetPropertyStr(ctx, global, "__ccordParentOrigin", JS_NewString(ctx, f->parent ? f->parent->url.origin : ""));
  JS_SetPropertyStr(ctx, global, "__ccordUA", JS_NewString(ctx, f->view->user_agent));
  JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewFloat64(ctx, (double)f->viewport_w));
  JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewFloat64(ctx, (double)f->viewport_h));
  JS_SetPropertyStr(ctx, global, "outerWidth", JS_NewFloat64(ctx, (double)f->viewport_w));
  JS_SetPropertyStr(ctx, global, "outerHeight", JS_NewFloat64(ctx, (double)f->viewport_h));
  JS_SetPropertyStr(ctx, global, "devicePixelRatio", JS_NewFloat64(ctx, 1.0));

  // Console
  JSValue con = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, con, "log", JS_NewCFunctionMagic(ctx, js_console_fn, "log", 1, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, con, "info", JS_NewCFunctionMagic(ctx, js_console_fn, "info", 1, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, con, "warn", JS_NewCFunctionMagic(ctx, js_console_fn, "warn", 1, JS_CFUNC_generic_magic, 1));
  JS_SetPropertyStr(ctx, con, "error", JS_NewCFunctionMagic(ctx, js_console_fn, "error", 1, JS_CFUNC_generic_magic, 2));
  JS_SetPropertyStr(ctx, con, "debug", JS_NewCFunctionMagic(ctx, js_console_fn, "debug", 1, JS_CFUNC_generic_magic, 3));
  JS_SetPropertyStr(ctx, global, "console", con);

  // Performance
  JSValue perf = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, perf, "now", JS_NewCFunction(ctx, js_perf_now, "now", 0));
  JS_SetPropertyStr(ctx, perf, "timeOrigin", JS_NewFloat64(ctx, (double)ccord_clock_unix_ms()));
  JS_SetPropertyStr(ctx, global, "performance", perf);

  // Crypto
  JSValue cr = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, cr, "getRandomValues", JS_NewCFunction(ctx, js_crypto_random, "getRandomValues", 1));
  JS_SetPropertyStr(ctx, cr, "randomUUID", JS_NewCFunction(ctx, js_crypto_uuid, "randomUUID", 0));
  JS_SetPropertyStr(ctx, global, "crypto", cr);

  // Trusted top-level host bridge ONLY (never exposed in cross-origin iframes)
  if (f->is_root) {
    JSValue cc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cc, "postMessage", JS_NewCFunction(ctx, js_ccord_host_post, "postMessage", 2));
    JS_SetPropertyStr(ctx, global, "ccord", cc);
  }
  JS_FreeValue(ctx, global);

  // Install WebAssembly.* backed by wasm3
  web_wasm_install(f);

  // Standard Web Platform classes & helpers built atop our C primitives (each chunk <3.5KB for C23 string limit)
  static const char k_web_env_1a[] =
    "(function(g){"
    "  g.setTimeout = function(fn, ms, ...args){ return args.length && typeof fn === 'function' ? __ccordSetTimeout(() => fn(...args), ms) : __ccordSetTimeout(fn, ms); };"
    "  g.setInterval = function(fn, ms, ...args){ return args.length && typeof fn === 'function' ? __ccordSetInterval(() => fn(...args), ms) : __ccordSetInterval(fn, ms); };"
    "  g.queueMicrotask = fn => Promise.resolve().then(fn);"
    "  g.requestIdleCallback = cb => g.setTimeout(() => cb({ didTimeout: false, timeRemaining: () => 50 }), 1);"
    "  g.cancelIdleCallback = id => g.clearTimeout(id);"
    "  const _proxies = new Map();"
    "  g.__ccordGetFrameProxy = function(fid){"
    "    if (fid === __ccordFrameId) return g;"
    "    let p = _proxies.get(fid);"
    "    if (!p) {"
    "      p = { _ccordFrameId: fid, length: 0, opener: null, focus(){}, blur(){},"
    "        postMessage(d, o){ __ccordPostMsg(fid, d, o || '*'); },"
    "        get closed(){ return !__ccordFrameAlive(fid); },"
    "        location: { assign(){}, replace(){} } };"
    "      p.window = p; p.self = p; p.frames = p;"
    "      _proxies.set(fid, p);"
    "    }"
    "    return p;"
    "  };"
    "  g.postMessage = function(d, o){ __ccordPostMsg(__ccordIsWorker ? __ccordParentId : __ccordFrameId, d, o || '*'); };"
    "  g.parent = __ccordIsRoot ? g : g.__ccordGetFrameProxy(__ccordParentId);"
    "  g.top = __ccordIsRoot ? g : g.__ccordGetFrameProxy(__ccordTopId);"
    "  g.frames = g; g.length = 0; g.opener = null; g.frameElement = null; g.name = ''; g.closed = false;"
    "  g.scrollX = 0; g.scrollY = 0; g.pageXOffset = 0; g.pageYOffset = 0;"
    "  g.screenX = 0; g.screenY = 0; g.screenLeft = 0; g.screenTop = 0;"
    "  g.scrollTo = () => {}; g.scrollBy = () => {}; g.scroll = () => {}; g.focus = () => {}; g.blur = () => {}; g.close = () => {}; g.open = () => null;"
    "  g.getSelection = () => ({ toString: () => '', removeAllRanges(){}, addRange(){} });"
    "  g.structuredClone = x => (x instanceof Uint8Array ? new Uint8Array(x) : (x instanceof ArrayBuffer ? x.slice(0) : JSON.parse(JSON.stringify(x))));"
    "  g.isSecureContext = true;"
    "})(globalThis);";

  static const char k_web_env_1b[] =
    "(function(g){"
    "  const anc = __ccordParentOrigin ? [__ccordParentOrigin] : [];"
    "  const locState = { href:'', origin:'null', protocol:'https:', host:'', hostname:'', port:'', pathname:'/', search:'', hash:'' };"
    "  g.__ccordSyncLocation = function(href){"
    "    const p = __ccordParseUrl(String(href)) || locState;"
    "    Object.assign(locState, p); g.origin = locState.origin;"
    "  };"
    "  g.__ccordSyncLocation(__ccordInitialHref);"
    "  g.location = {"
    "    get href(){ return locState.href; }, set href(u){ g.__ccordSyncLocation(u); },"
    "    get origin(){ return locState.origin; }, get protocol(){ return locState.protocol; },"
    "    get host(){ return locState.host; }, get hostname(){ return locState.hostname; },"
    "    get port(){ return locState.port; }, get pathname(){ return locState.pathname; },"
    "    get search(){ return locState.search; },"
    "    get hash(){ return locState.hash; },"
    "    set hash(h){ const old = locState.href; const nh = h ? (String(h).startsWith('#') ? String(h) : '#' + h) : ''; locState.hash = nh; locState.href = locState.origin + locState.pathname + locState.search + nh; g.dispatchEvent({ type:'hashchange', oldURL:old, newURL:locState.href }); },"
    "    ancestorOrigins: anc, toString(){ return locState.href; }, assign(u){ g.__ccordSyncLocation(u); }, replace(u){ g.__ccordSyncLocation(u); }, reload(){}"
    "  };"
    "  if (g.document) Object.defineProperty(g.document, 'location', { get(){ return g.location; }, set(u){ g.location.href = u; }, configurable: true });"
    "  g.screen = { width: 1920, height: 1080, availWidth: 1920, availHeight: 1040, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0 } };"
    "  g.history = { length: 1, state: null, pushState(){}, replaceState(){}, back(){}, forward(){}, go(){} };"
    "  g.matchMedia = (q) => { const s = String(q || '');"
    "    let m = false;"
    "    if (/prefers-color-scheme:\\s*dark/i.test(s)) m = !!__ccordDarkTheme;"
    "    else if (/prefers-color-scheme:\\s*light/i.test(s)) m = !__ccordDarkTheme;"
    "    else if (/pointer:\\s*coarse/i.test(s) || /hover:\\s*none/i.test(s)) m = !!__ccordTouchEnabled;"
    "    else if (/pointer:\\s*fine/i.test(s) || /hover:\\s*hover/i.test(s)) m = !__ccordTouchEnabled;"
    "    return { matches: m, media: s, addListener(){}, removeListener(){}, addEventListener(){}, removeEventListener(){} }; };"
    "  const mkStor = () => ({ getItem: k => __ccordStorGet(String(k)), setItem: (k,v) => __ccordStorSet(String(k), String(v)), removeItem: k => __ccordStorDel(String(k)), clear(){}, get length(){ return 0; } });"
    "  g.localStorage = mkStor(); g.sessionStorage = mkStor();"
    "})(globalThis);";

  static const char k_web_env_1c[] =
    "(function(g){"
    "  g.navigator = { userAgent: __ccordUA, appVersion: __ccordUA.replace(/^Mozilla\\//,''), appName: 'Netscape',"
    "    platform: __ccordIsAndroid ? 'Linux armv8l' : 'Linux x86_64', language: 'en-US', languages: ['en-US', 'en'], onLine: true, cookieEnabled: true,"
    "    hardwareConcurrency: 8, deviceMemory: 8, maxTouchPoints: __ccordTouchEnabled ? 5 : 0, vendor: 'Google Inc.', webdriver: false,"
    "    doNotTrack: null, pdfViewerEnabled: true, plugins: [], mimeTypes: [],"
    "    connection: { effectiveType: '4g', rtt: 50, downlink: 10, saveData: false },"
    "    permissions: { query: () => Promise.resolve({ state: 'prompt' }) },"
    "    clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve('') },"
    "    mediaDevices: { enumerateDevices: () => Promise.resolve([]) },"
    "    storage: { estimate: () => Promise.resolve({ quota: 1073741824, usage: 0 }) },"
    "    sendBeacon: (u, d) => { try { g.fetch(u, { method: 'POST', body: d }); } catch(_){} return true; },"
    "    userAgentData: { brands: [{ brand: 'Chromium', version: '131' }], mobile: !!__ccordTouchEnabled, platform: __ccordIsAndroid ? 'Android' : 'Linux', getHighEntropyValues: () => Promise.resolve({ platform: __ccordIsAndroid ? 'Android' : 'Linux', architecture: __ccordIsAndroid ? 'arm' : 'x86' }) } };"
    "  if (__ccordTouchEnabled) { g.ontouchstart = null; g.ontouchmove = null; g.ontouchend = null; g.ontouchcancel = null; }"
    "  const t0 = Math.floor(g.performance.timeOrigin);"
    "  g.performance.timing = { navigationStart: t0, fetchStart: t0, domainLookupStart: t0, domainLookupEnd: t0, connectStart: t0, connectEnd: t0, requestStart: t0, responseStart: t0, responseEnd: t0, domLoading: t0, domInteractive: t0, domContentLoadedEventStart: t0, domContentLoadedEventEnd: t0, domComplete: t0, loadEventStart: t0, loadEventEnd: t0 };"
    "  g.performance.navigation = { type: 0, redirectCount: 0 };"
    "  g.performance.mark = () => {}; g.performance.measure = () => {}; g.performance.getEntries = () => []; g.performance.getEntriesByType = () => []; g.performance.getEntriesByName = () => [];"
    "  g.crypto.subtle = {"
    "    digest(alg, data){ try { const n = typeof alg === 'string' ? alg : alg.name; return Promise.resolve(__ccordDigest(n, data)); } catch(e){ return Promise.reject(e); } },"
    "    importKey(fmt, raw, alg, ext, uses){ return Promise.resolve({ fmt, raw: new Uint8Array(raw instanceof ArrayBuffer ? raw : raw.buffer), alg, ext, uses }); },"
    "    sign(alg, key, data){ try { const h = (key.alg && key.alg.hash && (key.alg.hash.name || key.alg.hash)) || 'SHA-256'; return Promise.resolve(__ccordHmac(h, key.raw, data)); } catch(e){ return Promise.reject(e); } }"
    "  };"
    "})(globalThis);";

  static const char k_web_env_2a[] =
    "(function(g){"
    "  g.TextEncoder = class TextEncoder {"
    "    encode(s=''){ const out=[]; s=String(s); for(let i=0;i<s.length;i++){ let cp=s.codePointAt(i); if(cp>0xFFFF) i++;"
    "      if(cp<0x80) out.push(cp); else if(cp<0x800) out.push(0xC0|(cp>>6), 0x80|(cp&0x3F));"
    "      else if(cp<0x10000) out.push(0xE0|(cp>>12), 0x80|((cp>>6)&0x3F), 0x80|(cp&0x3F));"
    "      else out.push(0xF0|(cp>>18), 0x80|((cp>>12)&0x3F), 0x80|((cp>>6)&0x3F), 0x80|(cp&0x3F)); } return new Uint8Array(out); }"
    "  };"
    "  g.TextDecoder = class TextDecoder {"
    "    decode(buf){ if(!buf) return ''; const u = buf instanceof Uint8Array ? buf : new Uint8Array(buf instanceof ArrayBuffer ? buf : buf.buffer, buf.byteOffset || 0, buf.byteLength ?? buf.length);"
    "      let out=''; for(let i=0;i<u.length;){ const b=u[i]; if(b<0x80){ out+=String.fromCharCode(b); i++; }"
    "      else if((b&0xE0)===0xC0 && i+1<u.length){ out+=String.fromCharCode(((b&31)<<6)|(u[i+1]&63)); i+=2; }"
    "      else if((b&0xF0)===0xE0 && i+2<u.length){ out+=String.fromCharCode(((b&15)<<12)|((u[i+1]&63)<<6)|(u[i+2]&63)); i+=3; }"
    "      else if((b&0xF8)===0xF0 && i+3<u.length){ out+=String.fromCodePoint(((b&7)<<18)|((u[i+1]&63)<<12)|((u[i+2]&63)<<6)|(u[i+3]&63)); i+=4; }"
    "      else { out+='\\uFFFD'; i++; } } return out; }"
    "  };"
    "  g.URLSearchParams = class URLSearchParams {"
    "    constructor(init=''){ this._p=[];"
    "      if(typeof init==='string'){ const s=init.replace(/^\\?/,''); if(s) for(const kv of s.split('&')){ const idx=kv.indexOf('='); const k=decodeURIComponent((idx<0?kv:kv.slice(0,idx)).replace(/\\+/g,' ')); const v=idx<0?'':decodeURIComponent(kv.slice(idx+1).replace(/\\+/g,' ')); this._p.push([k,v]); } }"
    "      else if(init instanceof URLSearchParams){ for(const [k,v] of init._p) this._p.push([k,v]); }"
    "      else if(Array.isArray(init)){ for(const it of init) if(it && it.length>=2) this._p.push([String(it[0]),String(it[1])]); }"
    "      else if(init && typeof init==='object'){ for(const [k,v] of Object.entries(init)) this._p.push([String(k),String(v)]); } }"
    "    get size(){ return this._p.length; }"
    "    get(k){ const e=this._p.find(x=>x[0]===String(k)); return e?e[1]:null; }"
    "    getAll(k){ const sk=String(k); return this._p.filter(x=>x[0]===sk).map(x=>x[1]); }"
    "    has(k){ return this._p.some(x=>x[0]===String(k)); }"
    "    set(k,v){ this.delete(k); this._p.push([String(k),String(v)]); }"
    "    append(k,v){ this._p.push([String(k),String(v)]); }"
    "    delete(k){ this._p=this._p.filter(x=>x[0]!==String(k)); }"
    "    sort(){ this._p.sort((a,b)=>a[0]<b[0]?-1:(a[0]>b[0]?1:0)); }"
    "    entries(){ return this._p[Symbol.iterator](); }"
    "    keys(){ return this._p.map(x=>x[0])[Symbol.iterator](); }"
    "    values(){ return this._p.map(x=>x[1])[Symbol.iterator](); }"
    "    forEach(cb, th){ for(const [k,v] of this._p) cb.call(th, v, k, this); }"
    "    toString(){ return this._p.map(([k,v])=>encodeURIComponent(k)+'='+encodeURIComponent(v)).join('&'); }"
    "    [Symbol.iterator](){ return this._p[Symbol.iterator](); }"
    "  };"
    "})(globalThis);";

  static const char k_web_env_2b[] =
    "(function(g){"
    "  g.URL = class URL {"
    "    constructor(u, base){ const p=__ccordParseUrl(String(u), base ? String(base) : undefined); if(!p) throw new TypeError('Invalid URL: '+u); Object.assign(this, p); this.searchParams=new URLSearchParams(p.search); }"
    "    toString(){ return this.href; } toJSON(){ return this.href; }"
    "    static createObjectURL(b){ return __ccordBlobCreate(b && b._data !== undefined ? b._data : String(b), (b && b.type) || 'application/octet-stream'); }"
    "    static revokeObjectURL(){}"
    "  };"
    "  g.Blob = class Blob {"
    "    constructor(parts=[], opts={}){ this.type=opts.type||''; const enc=new TextEncoder(); const chunks=[]; let tot=0;"
    "      for(const p of parts){ const u = p instanceof Uint8Array ? p : (p instanceof ArrayBuffer ? new Uint8Array(p) : (p instanceof Blob ? p._data : enc.encode(String(p)))); chunks.push(u); tot+=u.length; }"
    "      this._data=new Uint8Array(tot); let o=0; for(const c of chunks){ this._data.set(c,o); o+=c.length; } this.size=tot; }"
    "    text(){ return Promise.resolve(new TextDecoder().decode(this._data)); }"
    "    arrayBuffer(){ return Promise.resolve(this._data.buffer.slice(0)); }"
    "    slice(s=0, e=this.size, t=''){ return new Blob([this._data.slice(s, e)], { type: t }); }"
    "  };"
    "  g.Headers = class Headers {"
    "    constructor(init){ this._m=new Map(); if(typeof init==='string'){ for(const line of init.split(/\\r?\\n/)){ const i=line.indexOf(':'); if(i>0) this.set(line.slice(0,i).trim(), line.slice(i+1).trim()); } }"
    "      else if(init && typeof init==='object'){ for(const [k,v] of (init instanceof Headers ? init._m.entries() : Object.entries(init))) this.set(k,v); } }"
    "    get(k){ return this._m.get(String(k).toLowerCase()) ?? null; }"
    "    has(k){ return this._m.has(String(k).toLowerCase()); }"
    "    set(k,v){ this._m.set(String(k).toLowerCase(), String(v)); }"
    "    append(k,v){ const lk=String(k).toLowerCase(); this._m.set(lk, this._m.has(lk) ? this._m.get(lk)+', '+v : String(v)); }"
    "    delete(k){ this._m.delete(String(k).toLowerCase()); }"
    "    forEach(cb){ this._m.forEach((v,k)=>cb(v,k,this)); }"
    "    entries(){ return this._m.entries(); } keys(){ return this._m.keys(); } values(){ return this._m.values(); }"
    "    [Symbol.iterator](){ return this._m.entries(); }"
    "  };"
    "  g.FormData = class FormData {"
    "    constructor(){ this._p=[]; }"
    "    append(k,v){ this._p.push([String(k), v]); } set(k,v){ this.delete(k); this._p.push([String(k), v]); }"
    "    get(k){ const e=this._p.find(x=>x[0]===String(k)); return e?e[1]:null; }"
    "    has(k){ return this._p.some(x=>x[0]===String(k)); } delete(k){ this._p=this._p.filter(x=>x[0]!==String(k)); }"
    "    forEach(cb){ for(const [k,v] of this._p) cb(v,k,this); } entries(){ return this._p[Symbol.iterator](); } [Symbol.iterator](){ return this._p[Symbol.iterator](); }"
    "  };"
    "})(globalThis);";

  static const char k_web_env_3a[] =
    "(function(g){"
    "  class Request { constructor(u, init={}){ this.url=typeof u==='string'?u:(u&&u.url?u.url:String(u)); this.method=(init.method||'GET').toUpperCase(); this.headers=new Headers(init.headers||{}); this.body=init.body??null; } }"
    "  g.Request = Request;"
    "  class Response {"
    "    constructor(raw){ this.status=raw.status; this.statusText=raw.status>=200&&raw.status<300?'OK':''; this.ok=raw.status>=200&&raw.status<300; this.url=raw.url; this.redirected=false; this.headers=new Headers(raw.headersRaw||''); this._buf=raw.bodyBuf; this._txt=raw.bodyText||''; this.type='cors'; }"
    "    text(){ return Promise.resolve(this._txt); }"
    "    json(){ return Promise.resolve().then(()=>JSON.parse(this._txt)); }"
    "    arrayBuffer(){ return Promise.resolve(this._buf.slice(0)); }"
    "    blob(){ return Promise.resolve(new Blob([new Uint8Array(this._buf)], {type:this.headers.get('content-type')||''})); }"
    "    clone(){ return new Response({status:this.status,url:this.url,headersRaw:'',bodyBuf:this._buf,bodyText:this._txt}); }"
    "  }"
    "  g.Response = Response;"
    "  g.fetch = function(input, init={}){"
    "    const url = typeof input === 'string' ? input : (input && input.url ? input.url : String(input));"
    "    const method = (init && init.method) || (input && input.method) || 'GET';"
    "    const hdrs = [];"
    "    const rawH = (init && init.headers) || (input && input.headers);"
    "    if (rawH) { const h = new Headers(rawH); h.forEach((v,k)=>{ hdrs.push(k, v); }); }"
    "    let body = (init && init.body !== undefined) ? init.body : (input && input.body);"
    "    if (body instanceof URLSearchParams) body = body.toString();"
    "    return __ccordFetch(url, method, hdrs, body).then(r => new Response(r));"
    "  };"
    "  class XMLHttpRequest {"
    "    constructor(){ this.readyState=0; this.status=0; this.statusText=''; this.responseText=''; this.response=''; this.responseType=''; this.responseURL=''; this.withCredentials=false; this.timeout=0; this.upload={addEventListener(){},removeEventListener(){}}; this._hdrs=[]; this._rh=new Headers(); this._ls={}; }"
    "    open(m, u){ this._m=m||'GET'; this._u=String(u); this._hdrs=[]; this.readyState=1; this._fire('readystatechange'); }"
    "    setRequestHeader(k,v){ this._hdrs.push(String(k), String(v)); }"
    "    overrideMimeType(){}"
    "    getResponseHeader(k){ return this._rh.get(k); }"
    "    getAllResponseHeaders(){ return this._rawH || ''; }"
    "    addEventListener(t, fn){ (this._ls[t] = this._ls[t] || []).push(fn); }"
    "    removeEventListener(t, fn){ if(this._ls[t]) this._ls[t]=this._ls[t].filter(x=>x!==fn); }"
    "    _fire(t){ const e={type:t,target:this,currentTarget:this}; if(typeof this['on'+t]==='function') this['on'+t](e); if(this._ls[t]) for(const fn of this._ls[t]) fn.call(this,e); }"
    "    send(body){ if(body instanceof URLSearchParams) body = body.toString(); __ccordXhrSend(this, this._m, this._u, this._hdrs, body); }"
    "    abort(){}"
    "    __complete(ok, st, rawH, txt, buf, url){ this.status=st; this.statusText=st>=200&&st<300?'OK':''; this.responseURL=url||''; this._rawH=rawH||''; this._rh=new Headers(rawH||''); this.responseText=txt||''; this.response = this.responseType==='arraybuffer' ? buf : (this.responseType==='json' ? (function(){try{return JSON.parse(txt);}catch(_){return null;}})() : this.responseText); this.readyState=4; this._fire('readystatechange'); this._fire(ok ? 'load' : 'error'); this._fire('loadend'); }"
    "  }"
    "  XMLHttpRequest.UNSENT=0; XMLHttpRequest.OPENED=1; XMLHttpRequest.HEADERS_RECEIVED=2; XMLHttpRequest.LOADING=3; XMLHttpRequest.DONE=4;"
    "  Object.assign(XMLHttpRequest.prototype, { UNSENT:0, OPENED:1, HEADERS_RECEIVED:2, LOADING:3, DONE:4 });"
    "  g.XMLHttpRequest = XMLHttpRequest;"
    "})(globalThis);";

  static const char k_web_env_3b[] =
    "(function(g){"
    "  g.DOMException = class DOMException extends Error { constructor(m='', n='Error'){ super(m); this.name=n; } };"
    "  g.CSS = { supports(){ return true; }, escape(s){ return String(s); } };"
    "  g.Event = class Event { constructor(t,o={}){ this.type=t; this.bubbles=!!o.bubbles; this.cancelable=!!o.cancelable; this.defaultPrevented=false; this.isTrusted=false; this._stopProp=false; this._stopImm=false; this.timeStamp=Date.now(); }"
    "    preventDefault(){ this.defaultPrevented=true; } stopPropagation(){ this._stopProp=true; } stopImmediatePropagation(){ this._stopProp=true; this._stopImm=true; } };"
    "  g.CustomEvent = class CustomEvent extends g.Event { constructor(t,o={}){ super(t,o); this.detail=o.detail??null; } };"
    "  g.MouseEvent = class MouseEvent extends g.Event { constructor(t,o={}){ super(t,o); Object.assign(this,o); } };"
    "  g.PointerEvent = class PointerEvent extends g.MouseEvent {};"
    "  g.KeyboardEvent = class KeyboardEvent extends g.Event { constructor(t,o={}){ super(t,o); Object.assign(this,o); } };"
    "  g.FocusEvent = class FocusEvent extends g.Event { constructor(t,o={}){ super(t,o); Object.assign(this,o); } };"
    "  g.MessageEvent = class MessageEvent extends g.Event { constructor(t,o={}){ super(t,o); this.data=o.data; this.origin=o.origin||''; this.source=o.source||null; this.ports=o.ports||[]; } };"
    "  g.AbortController = class AbortController { constructor(){ this.signal={aborted:false, addEventListener(){}, removeEventListener(){}}; } abort(){ this.signal.aborted=true; } };"
    "  g.MutationObserver = class MutationObserver { constructor(cb){ this._cb=cb; } observe(t,o){ __ccordMutObserve(this._cb, t, o||{}); } disconnect(){} takeRecords(){ return []; } };"
    "  g.ResizeObserver = class ResizeObserver { constructor(cb){ this._cb=cb; this._els=new Set(); this._onRes=()=>{ this._els.forEach(el=>{ try{ const r=el.getBoundingClientRect(); this._cb([{target:el,contentRect:r,borderBoxSize:[{inlineSize:r.width,blockSize:r.height}],contentBoxSize:[{inlineSize:r.width,blockSize:r.height}]}], this); }catch(_){} }); }; g.addEventListener('resize', this._onRes); }"
    "    observe(el){ if(!el) return; this._els.add(el); if(typeof this._cb==='function') g.setTimeout(()=>{ try{ const r=el.getBoundingClientRect(); this._cb([{target:el,contentRect:r,borderBoxSize:[{inlineSize:r.width,blockSize:r.height}],contentBoxSize:[{inlineSize:r.width,blockSize:r.height}]}], this); }catch(_){} }, 0); }"
    "    unobserve(el){ this._els.delete(el); } disconnect(){ this._els.clear(); g.removeEventListener('resize', this._onRes); } };"
    "  g.IntersectionObserver = class IntersectionObserver { constructor(cb){ this._cb=cb; } observe(el){ if(el && typeof this._cb==='function') g.setTimeout(()=>{ try{ const r=el.getBoundingClientRect(); this._cb([{target:el,isIntersecting:true,intersectionRatio:1,boundingClientRect:r,intersectionRect:r,rootBounds:{x:0,y:0,left:0,top:0,width:g.innerWidth,height:g.innerHeight,right:g.innerWidth,bottom:g.innerHeight},time:g.performance.now()}], this); }catch(_){} }, 0); } unobserve(){} disconnect(){} takeRecords(){ return []; } };"
    "  g.MessageChannel = class MessageChannel { constructor(){"
    "    const mk=()=>({ _ls:{}, onmessage:null, start(){}, close(){}, addEventListener(t,f){(this._ls[t]=this._ls[t]||[]).push(f);},"
    "      postMessage(d){ const peer=this._peer; g.setTimeout(()=>{ const ev={type:'message',data:d,ports:[]}; if(typeof peer.onmessage==='function') peer.onmessage(ev); if(peer._ls.message) peer._ls.message.forEach(f=>f(ev)); }, 0); } });"
    "    this.port1=mk(); this.port2=mk(); this.port1._peer=this.port2; this.port2._peer=this.port1; } };"
    "  g.Worker = class Worker { constructor(u){ this._fid=__ccordWorkerSpawn(String(u), this); this.onmessage=null; this.onerror=null; }"
    "    postMessage(d){ if(this._fid) __ccordPostMsg(this._fid, d, '*'); } terminate(){} addEventListener(t,f){ if(t==='message') this.onmessage=f; } };"
    "})(globalThis);";

  static const char k_web_env_3c[] =
    "(function(g){"
    "  if (typeof g.Intl === 'undefined') {"
    "    const dtfOpts = { locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC', year: 'numeric', month: '2-digit', day: '2-digit' };"
    "    g.Intl = {"
    "      DateTimeFormat: function(l, o){ if(!(this instanceof g.Intl.DateTimeFormat)) return new g.Intl.DateTimeFormat(l, o); this._o = Object.assign({}, dtfOpts, o || {}); this.format = d => new Date(d || Date.now()).toISOString(); this.resolvedOptions = () => Object.assign({}, this._o); },"
    "      NumberFormat: function(l, o){ if(!(this instanceof g.Intl.NumberFormat)) return new g.Intl.NumberFormat(l, o); this.format = n => String(n); this.resolvedOptions = () => ({ locale: 'en-US', numberingSystem: 'latn' }); },"
    "      Collator: function(){ return { compare: (a,b) => String(a).localeCompare(String(b)), resolvedOptions: () => ({ locale: 'en-US' }) }; },"
    "      PluralRules: function(){ return { select: n => n === 1 ? 'one' : 'other', resolvedOptions: () => ({ locale: 'en-US' }) }; },"
    "      RelativeTimeFormat: function(){ return { format: (v, u) => v + ' ' + u, resolvedOptions: () => ({ locale: 'en-US' }) }; },"
    "      Locale: class Locale { constructor(t='en-US'){ this.baseName=String(t); this.language=this.baseName.split('-')[0]; } toString(){ return this.baseName; } }"
    "    };"
    "  }"
    "  g.Window = function Window(){}; Object.defineProperty(g.Window, Symbol.hasInstance, { value: x => x === g });"
    "  g.EventTarget = function EventTarget(){}; Object.defineProperty(g.EventTarget, Symbol.hasInstance, { value: x => x != null && typeof x.addEventListener === 'function' });"
    "  if (!__ccordIsWorker && g.document) {"
    "    const np = Object.getPrototypeOf(g.document);"
    "    const mkCtor = (name, pred) => { const fn = function(){}; fn.prototype = np; Object.defineProperty(fn, Symbol.hasInstance, { value: x => x != null && typeof x === 'object' && pred(x) }); g[name] = fn; };"
    "    mkCtor('Node', x => typeof x.nodeType === 'number');"
    "    mkCtor('Element', x => x.nodeType === 1);"
    "    mkCtor('HTMLElement', x => x.nodeType === 1);"
    "    mkCtor('SVGElement', x => x.nodeType === 1);"
    "    mkCtor('Document', x => x.nodeType === 9);"
    "    mkCtor('HTMLDocument', x => x.nodeType === 9);"
    "    mkCtor('DocumentFragment', x => x.nodeType === 11);"
    "    mkCtor('Text', x => x.nodeType === 3);"
    "    mkCtor('Comment', x => x.nodeType === 8);"
    "    mkCtor('HTMLCanvasElement', x => x.tagName === 'CANVAS');"
    "    mkCtor('HTMLIFrameElement', x => x.tagName === 'IFRAME');"
    "    mkCtor('HTMLImageElement', x => x.tagName === 'IMG');"
    "    mkCtor('HTMLScriptElement', x => x.tagName === 'SCRIPT');"
    "    mkCtor('HTMLStyleElement', x => x.tagName === 'STYLE');"
    "    mkCtor('HTMLLinkElement', x => x.tagName === 'LINK');"
    "    mkCtor('HTMLDivElement', x => x.tagName === 'DIV');"
    "    mkCtor('HTMLInputElement', x => x.tagName === 'INPUT');"
    "    mkCtor('HTMLTextAreaElement', x => x.tagName === 'TEXTAREA');"
    "    mkCtor('HTMLButtonElement', x => x.tagName === 'BUTTON');"
    "    mkCtor('HTMLAnchorElement', x => x.tagName === 'A');"
    "    g.Node.ELEMENT_NODE=1; g.Node.TEXT_NODE=3; g.Node.COMMENT_NODE=8; g.Node.DOCUMENT_NODE=9; g.Node.DOCUMENT_FRAGMENT_NODE=11;"
    "    g.Image = function(w,h){ const img=g.document.createElement('img'); if(w) img.width=w; if(h) img.height=h; return img; };"
    "    g.DOMParser = class DOMParser { parseFromString(s){ const d=g.document.createElement('div'); d.innerHTML=s; return d; } };"
    "  }"
    "})(globalThis);";

  static const char k_web_env_4a[] =
    "(function(g){"
    "  g.UIEvent = class UIEvent extends g.Event { constructor(t,o={}){ super(t,o); Object.assign(this,o); } };"
    "  g.WheelEvent = class WheelEvent extends g.MouseEvent {}; g.TouchEvent = class TouchEvent extends g.UIEvent {};"
    "  g.InputEvent = class InputEvent extends g.UIEvent {}; g.DragEvent = class DragEvent extends g.MouseEvent {};"
    "  g.TransitionEvent = class TransitionEvent extends g.Event {}; g.AnimationEvent = class AnimationEvent extends g.Event {};"
    "  g.ErrorEvent = class ErrorEvent extends g.Event {}; g.ProgressEvent = class ProgressEvent extends g.Event {};"
    "  g.HashChangeEvent = class HashChangeEvent extends g.Event {}; g.PromiseRejectionEvent = class PromiseRejectionEvent extends g.Event {};"
    "  g.AbortSignal = { abort(r){ return {aborted:true,reason:r,addEventListener(){},removeEventListener(){}}; },"
    "    timeout(ms){ const s={aborted:false,addEventListener(){},removeEventListener(){}}; g.setTimeout(()=>{ s.aborted=true; }, ms); return s; },"
    "    any(list){ return (list && list[0]) || {aborted:false,addEventListener(){},removeEventListener(){}}; } };"
    "  g.PerformanceObserver = class PerformanceObserver { constructor(cb){ this._cb=cb; } observe(){} disconnect(){} takeRecords(){ return []; } };"
    "  g.CSSStyleSheet = class CSSStyleSheet { constructor(){ this._css=''; this.cssRules=[]; } replaceSync(c){ this._css=String(c||''); if(g.document&&g.document.__styleInsertRule) g.document.__styleInsertRule(this._css); } replace(c){ this.replaceSync(c); return Promise.resolve(this); } insertRule(r){ this.cssRules.push({cssText:String(r)}); if(g.document&&g.document.__styleInsertRule) g.document.__styleInsertRule(String(r)); return this.cssRules.length-1; } deleteRule(){} };"
    "  g.DOMRect = class DOMRect { constructor(x=0,y=0,w=0,h=0){ this.x=this.left=x; this.y=this.top=y; this.width=w; this.height=h; this.right=x+w; this.bottom=y+h; } static fromRect(o={}){ return new DOMRect(o.x||0,o.y||0,o.width||0,o.height||0); } };"
    "  g.DOMRectReadOnly = g.DOMRect;"
    "  g.DOMPoint = class DOMPoint { constructor(x=0,y=0,z=0,w=1){ this.x=x; this.y=y; this.z=z; this.w=w; } };"
    "  g.DOMPointReadOnly = g.DOMPoint;"
    "  g.DOMMatrix = class DOMMatrix { constructor(init){ this.a=this.m11=1; this.b=this.m12=0; this.c=this.m21=0; this.d=this.m22=1; this.e=this.m41=0; this.f=this.m42=0;"
    "    if(Array.isArray(init)&&init.length>=6){ [this.a,this.b,this.c,this.d,this.e,this.f]=init; this.m11=this.a; this.m12=this.b; this.m21=this.c; this.m22=this.d; this.m41=this.e; this.m42=this.f; } }"
    "    translate(tx=0,ty=0){ const m=new DOMMatrix([this.a,this.b,this.c,this.d,this.e+this.a*tx+this.c*ty,this.f+this.b*tx+this.d*ty]); return m; }"
    "    scale(sx=1,sy=sx){ return new DOMMatrix([this.a*sx,this.b*sx,this.c*sy,this.d*sy,this.e,this.f]); }"
    "    invertSelf(){ const det=this.a*this.d-this.b*this.c||1; const a=this.d/det,b=-this.b/det,c=-this.c/det,d=this.a/det,e=(this.c*this.f-this.d*this.e)/det,f=(this.b*this.e-this.a*this.f)/det; Object.assign(this,{a,b,c,d,e,f,m11:a,m12:b,m21:c,m22:d,m41:e,m42:f}); return this; }"
    "    inverse(){ return new DOMMatrix([this.a,this.b,this.c,this.d,this.e,this.f]).invertSelf(); }"
    "    transformPoint(p={x:0,y:0}){ return new g.DOMPoint(this.a*p.x+this.c*p.y+this.e, this.b*p.x+this.d*p.y+this.f); } };"
    "  g.DOMMatrixReadOnly = g.DOMMatrix;"
    "  g.ImageData = class ImageData { constructor(a,b,c){ if(a instanceof Uint8ClampedArray){ this.data=a; this.width=b|0; this.height=(c!==undefined?c:(a.length/(4*Math.max(1,this.width))))|0; } else { this.width=Math.max(1,a|0); this.height=Math.max(1,b|0); this.data=new Uint8ClampedArray(this.width*this.height*4); } } };"
    "})(globalThis);";

  static const char k_web_env_4b[] =
    "(function(g){"
    "  g.Path2D = class Path2D { constructor(p){ this._ops=[]; if(p instanceof Path2D) this._ops=p._ops.slice(); else if(typeof p==='string') this._parseSvg(p); }"
    "    _parseSvg(d){ const tok=d.match(/[a-zA-Z]|[-+]?(?:\\d*\\.\\d+|\\d+)(?:[eE][-+]?\\d+)?/g)||[]; let i=0,cx=0,cy=0,cmd=''; const num=()=>parseFloat(tok[i++]||'0');"
    "      while(i<tok.length){ if(/^[a-zA-Z]$/.test(tok[i])) cmd=tok[i++];"
    "        if(cmd==='M'||cmd==='m'){ let x=num(),y=num(); if(cmd==='m'){x+=cx;y+=cy;} cx=x;cy=y; this.moveTo(cx,cy); cmd=cmd==='M'?'L':'l'; }"
    "        else if(cmd==='L'||cmd==='l'){ let x=num(),y=num(); if(cmd==='l'){x+=cx;y+=cy;} cx=x;cy=y; this.lineTo(cx,cy); }"
    "        else if(cmd==='H'||cmd==='h'){ let x=num(); cx=cmd==='h'?cx+x:x; this.lineTo(cx,cy); }"
    "        else if(cmd==='V'||cmd==='v'){ let y=num(); cy=cmd==='v'?cy+y:y; this.lineTo(cx,cy); }"
    "        else if(cmd==='C'||cmd==='c'){ let x1=num(),y1=num(),x2=num(),y2=num(),x=num(),y=num(); if(cmd==='c'){x1+=cx;y1+=cy;x2+=cx;y2+=cy;x+=cx;y+=cy;} cx=x;cy=y; this.bezierCurveTo(x1,y1,x2,y2,x,y); }"
    "        else if(cmd==='Q'||cmd==='q'){ let x1=num(),y1=num(),x=num(),y=num(); if(cmd==='q'){x1+=cx;y1+=cy;x+=cx;y+=cy;} cx=x;cy=y; this.quadraticCurveTo(x1,y1,x,y); }"
    "        else if(cmd==='Z'||cmd==='z'){ this.closePath(); cmd=''; } else break; } }"
    "    moveTo(x,y){ this._ops.push(['moveTo',x,y]); } lineTo(x,y){ this._ops.push(['lineTo',x,y]); }"
    "    bezierCurveTo(a,b,c,d,e,f){ this._ops.push(['bezierCurveTo',a,b,c,d,e,f]); } quadraticCurveTo(a,b,c,d){ this._ops.push(['quadraticCurveTo',a,b,c,d]); }"
    "    arcTo(a,b,c,d,r){ this._ops.push(['arcTo',a,b,c,d,r]); } rect(x,y,w,h){ this._ops.push(['rect',x,y,w,h]); }"
    "    roundRect(x,y,w,h,r){ this._ops.push(['roundRect',x,y,w,h,r]); } arc(x,y,r,s,e,cc){ this._ops.push(['arc',x,y,r,s,e,cc]); }"
    "    ellipse(x,y,rx,ry,ro,s,e,cc){ this._ops.push(['ellipse',x,y,rx,ry,ro,s,e,cc]); } closePath(){ this._ops.push(['closePath']); }"
    "    addPath(p){ if(p&&p._ops) this._ops.push(...p._ops); }"
    "    __replayTo(ctx){ ctx.beginPath(); for(const op of this._ops){ const fn=ctx[op[0]]; if(typeof fn==='function') fn.apply(ctx, op.slice(1)); } } };"
    "})(globalThis);";

  static const char k_web_env_4c[] =
    "(function(g){"
    "  g.OffscreenCanvas = class OffscreenCanvas { constructor(w=300,h=150){ this._canvas=g.document?g.document.createElement('canvas'):null; if(this._canvas){ this._canvas.width=w; this._canvas.height=h; } }"
    "    get width(){ return this._canvas?this._canvas.width:300; } set width(v){ if(this._canvas) this._canvas.width=v; }"
    "    get height(){ return this._canvas?this._canvas.height:150; } set height(v){ if(this._canvas) this._canvas.height=v; }"
    "    getContext(t,o){ return this._canvas?this._canvas.getContext(t,o):null; }"
    "    transferToImageBitmap(){ return { width:this.width, height:this.height, _canvas:this._canvas, close(){} }; }"
    "    convertToBlob(o={}){ return new Promise(res=>{ if(this._canvas) this._canvas.toBlob(res, o.type||'image/png'); else res(new g.Blob([])); }); } };"
    "  g.createImageBitmap = function(src){ if(!src) return Promise.reject(new TypeError('empty source'));"
    "    if(src instanceof g.Blob && g.Image){ return new Promise((res,rej)=>{ const img=new g.Image(); img.onload=()=>res({width:img.naturalWidth||img.width,height:img.naturalHeight||img.height,_img:img,close(){}}); img.onerror=()=>rej(new Error('decode failed')); img.src=g.URL.createObjectURL(src); }); }"
    "    return Promise.resolve({ width:src.naturalWidth||src.width||0, height:src.naturalHeight||src.height||0, _canvas:src._canvas||src, _img:src, close(){} }); };"
    "  g.FileReader = class FileReader { constructor(){ this.readyState=0; this.result=null; this.onload=null; this.onloadend=null; this.onerror=null; this._ls={}; }"
    "    addEventListener(t,f){ (this._ls[t]=this._ls[t]||[]).push(f); } removeEventListener(t,f){ if(this._ls[t]) this._ls[t]=this._ls[t].filter(x=>x!==f); }"
    "    _done(r){ this.readyState=2; this.result=r; const e={type:'load',target:this}; if(typeof this.onload==='function') this.onload(e); if(this._ls.load) this._ls.load.forEach(f=>f(e)); if(typeof this.onloadend==='function') this.onloadend(e); }"
    "    readAsArrayBuffer(b){ b.arrayBuffer().then(ab=>this._done(ab)); }"
    "    readAsText(b){ b.text().then(t=>this._done(t)); }"
    "    readAsDataURL(b){ b.arrayBuffer().then(ab=>{ const u=new Uint8Array(ab); let s=''; for(let i=0;i<u.length;i++) s+=String.fromCharCode(u[i]); this._done('data:'+(b.type||'application/octet-stream')+';base64,'+g.btoa(s)); }); } };"
    "})(globalThis);";

  const char *chunks[11] = {k_web_env_1a, k_web_env_1b, k_web_env_1c, k_web_env_2a, k_web_env_2b, k_web_env_3a, k_web_env_3b, k_web_env_3c, k_web_env_4a, k_web_env_4b, k_web_env_4c};
  const char *names[11] = {"<web-env-1a>", "<web-env-1b>", "<web-env-1c>", "<web-env-2a>", "<web-env-2b>", "<web-env-3a>", "<web-env-3b>", "<web-env-3c>", "<web-env-4a>", "<web-env-4b>", "<web-env-4c>"};
  for (int i = 0; i < 11; i++) {
    JSValue rv = JS_Eval(ctx, chunks[i], strlen(chunks[i]), names[i], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(rv)) web_js_log_exception(f, names[i]);
    JS_FreeValue(ctx, rv);
  }
}
