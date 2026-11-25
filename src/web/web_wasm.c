// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#define _POSIX_C_SOURCE 200809L
#include "web_internal.h"

#include <wasm3.h>
#include <m3_env.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WASM_STACK_BYTES (256u * 1024u)

typedef struct web_wasm_module {
  uint8_t *bytes;
  size_t len;
} web_wasm_module;

typedef struct web_wasm_import_binding {
  JSContext *ctx;
  JSValue fn;
  IM3Function m3_fn;
} web_wasm_import_binding;

typedef struct web_wasm_instance {
  JSContext *ctx;
  IM3Environment env;
  IM3Runtime rt;
  IM3Module mod;
  uint8_t *bytes; // wasm3 requires module bytes to outlive IM3Module
  size_t len;
  web_wasm_import_binding *imports;
  size_t nimports;
} web_wasm_instance;

typedef struct web_wasm_memory {
  IM3Module mod;     // non-null when backed by a loaded wasm3 module's memory 0
  JSValue owner_inst; // strong ref keeping WebAssembly.Instance alive while Memory exists
  uint32_t mem_idx;
  uint8_t *owned;    // non-null for standalone new WebAssembly.Memory(...)
  size_t owned_bytes;
  uint32_t max_pages;
} web_wasm_memory;

static ccord_web_view *ctx_view(JSContext *ctx)
{
  web_frame *f = JS_GetContextOpaque(ctx);
  return f ? f->view : nullptr;
}

static bool extract_buffer_bytes(JSContext *ctx, JSValueConst val, const uint8_t **out_ptr, size_t *out_len)
{
  size_t sz = 0;
  uint8_t *p = JS_GetArrayBuffer(ctx, &sz, val);
  if (p) {
    *out_ptr = p;
    *out_len = sz;
    return true;
  }
  size_t off = 0, blen = 0, bpe = 0;
  JSValue ab = JS_GetTypedArrayBuffer(ctx, val, &off, &blen, &bpe);
  if (!JS_IsException(ab)) {
    p = JS_GetArrayBuffer(ctx, &sz, ab);
    JS_FreeValue(ctx, ab);
    if (p && off + blen <= sz) {
      *out_ptr = p + off;
      *out_len = blen;
      return true;
    }
  } else {
    JSValue ex = JS_GetException(ctx);
    JS_FreeValue(ctx, ex);
  }
  return false;
}

// ---- Finalizers ----

static void wasm_mod_finalizer(JSRuntime *rt, JSValue val)
{
  (void)rt;
  // Find class id via opaque; safe because only registered on cls_wasm_mod
  // QuickJS passes opaque via JS_GetOpaque2/JS_GetOpaque; we store view on RT opaque.
  ccord_web_view *v = JS_GetRuntimeOpaque(rt);
  if (!v) return;
  web_wasm_module *m = JS_GetOpaque(val, v->cls_wasm_mod);
  if (m) {
    ccord_free(m->bytes);
    ccord_free(m);
  }
}

static void wasm_inst_finalizer(JSRuntime *rt, JSValue val)
{
  ccord_web_view *v = JS_GetRuntimeOpaque(rt);
  if (!v) return;
  web_wasm_instance *inst = JS_GetOpaque(val, v->cls_wasm_inst);
  if (!inst) return;
  for (size_t i = 0; i < inst->nimports; i++) {
    JS_FreeValueRT(rt, inst->imports[i].fn);
  }
  ccord_free(inst->imports);
  if (inst->rt) m3_FreeRuntime(inst->rt);
  if (inst->env) m3_FreeEnvironment(inst->env);
  ccord_free(inst->bytes);
  ccord_free(inst);
}

static void wasm_inst_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
  ccord_web_view *v = JS_GetRuntimeOpaque(rt);
  if (!v) return;
  web_wasm_instance *inst = JS_GetOpaque(val, v->cls_wasm_inst);
  if (!inst) return;
  for (size_t i = 0; i < inst->nimports; i++) {
    JS_MarkValue(rt, inst->imports[i].fn, mark_func);
  }
}

static void wasm_mem_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
  ccord_web_view *v = JS_GetRuntimeOpaque(rt);
  if (!v) return;
  web_wasm_memory *mem = JS_GetOpaque(val, v->cls_wasm_mem);
  if (!mem) return;
  if (!JS_IsUndefined(mem->owner_inst)) JS_MarkValue(rt, mem->owner_inst, mark_func);
}

static void wasm_mem_finalizer(JSRuntime *rt, JSValue val)
{
  ccord_web_view *v = JS_GetRuntimeOpaque(rt);
  if (!v) return;
  web_wasm_memory *mem = JS_GetOpaque(val, v->cls_wasm_mem);
  if (!mem) return;
  if (!JS_IsUndefined(mem->owner_inst)) JS_FreeValueRT(rt, mem->owner_inst);
  ccord_free(mem->owned);
  ccord_free(mem);
}

// ---- WebAssembly.Memory ----

static JSValue wasm_mem_get_buffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)argc; (void)argv;
  ccord_web_view *v = ctx_view(ctx);
  if (!v) return JS_UNDEFINED;
  web_wasm_memory *mem = JS_GetOpaque(this_val, v->cls_wasm_mem);
  if (!mem) return JS_ThrowTypeError(ctx, "Invalid WebAssembly.Memory");
  uint8_t *ptr = nullptr;
  size_t sz = 0;
  if (mem->mod) {
    if (mem->mem_idx < mem->mod->numMemories && mem->mod->memories[mem->mem_idx]) {
      ptr = m3_GetMemory(mem->mod, &sz, mem->mem_idx);
    }
  } else {
    ptr = mem->owned;
    sz = mem->owned_bytes;
  }
  if (!ptr) return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
  // Zero-copy unmanaged view into wasm3's linear memory so JS & WASM share live memory!
  return JS_NewArrayBuffer(ctx, ptr, sz, 0, nullptr, nullptr, false);
}

static JSValue wasm_mem_grow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  ccord_web_view *v = ctx_view(ctx);
  if (!v) return JS_UNDEFINED;
  web_wasm_memory *mem = JS_GetOpaque(this_val, v->cls_wasm_mem);
  if (!mem) return JS_ThrowTypeError(ctx, "Invalid WebAssembly.Memory");
  uint32_t delta = 0;
  if (argc > 0) (void)JS_ToUint32(ctx, &delta, argv[0]);
  if (mem->mod) {
    if (mem->mem_idx >= mem->mod->numMemories || !mem->mod->memories[mem->mem_idx]) {
      return JS_ThrowRangeError(ctx, "WebAssembly.Memory not initialized");
    }
    size_t cur = m3_GetMemorySize(mem->mod, mem->mem_idx);
    uint32_t old_pages = (uint32_t)(cur / 65536u);
    if (delta == 0) return JS_NewUint32(ctx, old_pages);
    if (mem->mod->numMemories > mem->mem_idx && mem->mod->memories[mem->mem_idx]) {
      IM3Memory m = mem->mod->memories[mem->mem_idx];
      if (ResizeMemory(mem->mod->runtime, m, old_pages + delta) == m3Err_none) {
        return JS_NewUint32(ctx, old_pages);
      }
    }
    return JS_ThrowRangeError(ctx, "WebAssembly.Memory.grow failed");
  }
  uint32_t old_pages = (uint32_t)(mem->owned_bytes / 65536u);
  uint32_t new_pages = old_pages + delta;
  if (mem->max_pages && new_pages > mem->max_pages) {
    return JS_ThrowRangeError(ctx, "WebAssembly.Memory.grow exceeded maximum");
  }
  size_t new_bytes = (size_t)new_pages * 65536u;
  uint8_t *nb = ccord_realloc(mem->owned, new_bytes > 0 ? new_bytes : 1u);
  if (!nb) return JS_ThrowRangeError(ctx, "Out of memory");
  if (new_bytes > mem->owned_bytes) memset(nb + mem->owned_bytes, 0, new_bytes - mem->owned_bytes);
  mem->owned = nb;
  mem->owned_bytes = new_bytes;
  return JS_NewUint32(ctx, old_pages);
}

static JSValue make_wasm_mem_obj(JSContext *ctx, ccord_web_view *v, IM3Module mod, JSValueConst owner_inst,
                                 uint32_t idx, uint32_t init_pages, uint32_t max_pages)
{
  web_wasm_memory *mem = ccord_calloc(1, sizeof *mem);
  if (!mem) return JS_ThrowOutOfMemory(ctx);
  mem->mod = mod;
  mem->owner_inst = JS_IsUndefined(owner_inst) ? JS_UNDEFINED : JS_DupValue(ctx, owner_inst);
  mem->mem_idx = idx;
  mem->max_pages = max_pages;
  if (!mod) {
    size_t n = (size_t)init_pages * 65536u;
    mem->owned = ccord_calloc(1, n > 0 ? n : 1u);
    if (!mem->owned) {
      if (!JS_IsUndefined(mem->owner_inst)) JS_FreeValue(ctx, mem->owner_inst);
      ccord_free(mem);
      return JS_ThrowOutOfMemory(ctx);
    }
    mem->owned_bytes = n;
  }
  JSValue obj = JS_NewObjectClass(ctx, v->cls_wasm_mem);
  JS_SetOpaque(obj, mem);
  JSValue get_buf = JS_NewCFunction(ctx, wasm_mem_get_buffer, "get buffer", 0);
  JSAtom a_buf = JS_NewAtom(ctx, "buffer");
  JS_DefinePropertyGetSet(ctx, obj, a_buf, get_buf, JS_UNDEFINED, JS_PROP_CONFIGURABLE);
  JS_FreeAtom(ctx, a_buf);
  JS_SetPropertyStr(ctx, obj, "grow", JS_NewCFunction(ctx, wasm_mem_grow, "grow", 1));
  return obj;
}

static JSValue js_wasm_memory_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
  (void)new_target;
  ccord_web_view *v = ctx_view(ctx);
  if (!v) return JS_UNDEFINED;
  uint32_t initial = 1, maximum = 0;
  if (argc > 0 && JS_IsObject(argv[0])) {
    JSValue iv = JS_GetPropertyStr(ctx, argv[0], "initial");
    if (!JS_IsUndefined(iv)) (void)JS_ToUint32(ctx, &initial, iv);
    JS_FreeValue(ctx, iv);
    JSValue mv = JS_GetPropertyStr(ctx, argv[0], "maximum");
    if (!JS_IsUndefined(mv)) (void)JS_ToUint32(ctx, &maximum, mv);
    JS_FreeValue(ctx, mv);
  }
  return make_wasm_mem_obj(ctx, v, nullptr, JS_UNDEFINED, 0, initial, maximum);
}

// ---- Dynamic WASM <-> JS call trampolines ----

static const void *wasm_import_trampoline(IM3Runtime runtime, IM3ImportContext ctx_m3, uint64_t *_sp, void *_mem)
{
  (void)runtime;
  (void)_mem;
  web_wasm_import_binding *b = (web_wasm_import_binding *)ctx_m3->userdata;
  if (!b || !b->ctx || !b->m3_fn) return m3Err_none;

  uint32_t retc = m3_GetRetCount(b->m3_fn);
  uint32_t argc = m3_GetArgCount(b->m3_fn);
  JSValue js_args[16];
  uint32_t n = argc < 16u ? argc : 16u;
  for (uint32_t i = 0; i < n; i++) {
    uint64_t slot = _sp[retc + i];
    switch (m3_GetArgType(b->m3_fn, i)) {
      case c_m3Type_i32: js_args[i] = JS_NewInt32(b->ctx, (int32_t)slot); break;
      case c_m3Type_i64: js_args[i] = JS_NewBigInt64(b->ctx, (int64_t)slot); break;
      case c_m3Type_f32: {
        float fv = 0.0f;
        uint32_t u = (uint32_t)slot;
        memcpy(&fv, &u, sizeof fv);
        js_args[i] = JS_NewFloat64(b->ctx, (double)fv);
        break;
      }
      case c_m3Type_f64: {
        double dv = 0.0;
        memcpy(&dv, &slot, sizeof dv);
        js_args[i] = JS_NewFloat64(b->ctx, dv);
        break;
      }
      default: js_args[i] = JS_UNDEFINED; break;
    }
  }

  JSValue res = JS_Call(b->ctx, b->fn, JS_UNDEFINED, (int)n, js_args);
  for (uint32_t i = 0; i < n; i++) JS_FreeValue(b->ctx, js_args[i]);
  if (JS_IsException(res)) {
    JSValue ex = JS_GetException(b->ctx);
    JS_FreeValue(b->ctx, ex);
    return "JS import threw exception";
  }

  if (retc >= 1u) {
    switch (m3_GetRetType(b->m3_fn, 0)) {
      case c_m3Type_i32: {
        int32_t iv = 0;
        (void)JS_ToInt32(b->ctx, &iv, res);
        _sp[0] = (uint64_t)(uint32_t)iv;
        break;
      }
      case c_m3Type_i64: {
        int64_t iv = 0;
        (void)JS_ToBigInt64(b->ctx, &iv, res);
        _sp[0] = (uint64_t)iv;
        break;
      }
      case c_m3Type_f32: {
        double dv = 0.0;
        (void)JS_ToFloat64(b->ctx, &dv, res);
        float fv = (float)dv;
        uint32_t u = 0;
        memcpy(&u, &fv, sizeof u);
        _sp[0] = (uint64_t)u;
        break;
      }
      case c_m3Type_f64: {
        double dv = 0.0;
        (void)JS_ToFloat64(b->ctx, &dv, res);
        uint64_t u = 0;
        memcpy(&u, &dv, sizeof u);
        _sp[0] = u;
        break;
      }
      default:
        _sp[0] = 0;
        break;
    }
  }
  JS_FreeValue(b->ctx, res);
  return m3Err_none;
}

static char m3_type_char(M3ValueType t)
{
  switch (t) {
    case c_m3Type_i32: return 'i';
    case c_m3Type_i64: return 'I';
    case c_m3Type_f32: return 'f';
    case c_m3Type_f64: return 'F';
    default: return 'v';
  }
}

static void build_m3_sig(IM3Function fn, char out[64])
{
  size_t pos = 0;
  uint32_t retc = m3_GetRetCount(fn);
  if (retc == 0) {
    out[pos++] = 'v';
  } else {
    for (uint32_t i = 0; i < retc && pos < 16u; i++) out[pos++] = m3_type_char(m3_GetRetType(fn, i));
  }
  out[pos++] = '(';
  uint32_t argc = m3_GetArgCount(fn);
  for (uint32_t i = 0; i < argc && pos < 60u; i++) out[pos++] = m3_type_char(m3_GetArgType(fn, i));
  out[pos++] = ')';
  out[pos] = '\0';
}

static JSValue wasm_export_fn_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                                   int magic, JSValue *func_data)
{
  (void)this_val;
  (void)magic;
  ccord_web_view *v = ctx_view(ctx);
  if (!v) return JS_UNDEFINED;
  web_wasm_instance *inst = JS_GetOpaque(func_data[0], v->cls_wasm_inst);
  int32_t fn_idx = 0;
  (void)JS_ToInt32(ctx, &fn_idx, func_data[1]);
  if (!inst || !inst->mod || fn_idx < 0 || (uint32_t)fn_idx >= inst->mod->numFunctions) {
    return JS_ThrowInternalError(ctx, "Invalid WASM export function");
  }
  IM3Function fn = &inst->mod->functions[fn_idx];
  if (!fn->compiled) {
    return JS_ThrowInternalError(ctx, "WASM function not compiled/linked");
  }
  uint32_t nargs = m3_GetArgCount(fn);
  uint32_t nrets = m3_GetRetCount(fn);

  uint64_t arg_storage[16] = {0};
  const void *arg_ptrs[16] = {0};
  if (nargs > 16u) return JS_ThrowRangeError(ctx, "Too many WASM arguments");
  for (uint32_t i = 0; i < nargs; i++) {
    JSValueConst av = (i < (uint32_t)argc) ? argv[i] : JS_UNDEFINED;
    switch (m3_GetArgType(fn, i)) {
      case c_m3Type_i32: {
        int32_t iv = 0;
        (void)JS_ToInt32(ctx, &iv, av);
        memcpy(&arg_storage[i], &iv, sizeof iv);
        break;
      }
      case c_m3Type_i64: {
        int64_t iv = 0;
        if (JS_IsBigInt(av)) (void)JS_ToBigInt64(ctx, &iv, av);
        else (void)JS_ToInt64(ctx, &iv, av);
        memcpy(&arg_storage[i], &iv, sizeof iv);
        break;
      }
      case c_m3Type_f32: {
        double dv = 0.0;
        (void)JS_ToFloat64(ctx, &dv, av);
        float fv = (float)dv;
        memcpy(&arg_storage[i], &fv, sizeof fv);
        break;
      }
      case c_m3Type_f64: {
        double dv = 0.0;
        (void)JS_ToFloat64(ctx, &dv, av);
        memcpy(&arg_storage[i], &dv, sizeof dv);
        break;
      }
      default: break;
    }
    arg_ptrs[i] = &arg_storage[i];
  }

  M3Result res = m3_Call(fn, nargs, nargs > 0 ? arg_ptrs : nullptr);
  if (res != m3Err_none) {
    return JS_ThrowInternalError(ctx, "WASM trap: %s", res);
  }
  if (nrets == 0) return JS_UNDEFINED;

  uint64_t ret_storage[4] = {0};
  const void *ret_ptrs[4] = {&ret_storage[0], &ret_storage[1], &ret_storage[2], &ret_storage[3]};
  uint32_t nr = nrets < 4u ? nrets : 4u;
  res = m3_GetResults(fn, nr, ret_ptrs);
  if (res != m3Err_none) return JS_ThrowInternalError(ctx, "WASM result error: %s", res);

  switch (m3_GetRetType(fn, 0)) {
    case c_m3Type_i32: { int32_t iv = 0; memcpy(&iv, &ret_storage[0], sizeof iv); return JS_NewInt32(ctx, iv); }
    case c_m3Type_i64: { int64_t iv = 0; memcpy(&iv, &ret_storage[0], sizeof iv); return JS_NewBigInt64(ctx, iv); }
    case c_m3Type_f32: { float fv = 0.0f; memcpy(&fv, &ret_storage[0], sizeof fv); return JS_NewFloat64(ctx, (double)fv); }
    case c_m3Type_f64: { double dv = 0.0; memcpy(&dv, &ret_storage[0], sizeof dv); return JS_NewFloat64(ctx, dv); }
    default: return JS_UNDEFINED;
  }
}

// ---- Module & Instance construction ----

static JSValue create_wasm_module_obj(JSContext *ctx, ccord_web_view *v, const uint8_t *bytes, size_t len)
{
  IM3Environment env = m3_NewEnvironment();
  IM3Module mod = nullptr;
  M3Result r = m3_ParseModule(env, &mod, bytes, (uint32_t)len);
  if (mod) m3_FreeModule(mod);
  m3_FreeEnvironment(env);
  if (r != m3Err_none) {
    return JS_ThrowTypeError(ctx, "WebAssembly.CompileError: %s", r);
  }
  web_wasm_module *wm = ccord_calloc(1, sizeof *wm);
  if (!wm) return JS_ThrowOutOfMemory(ctx);
  wm->bytes = ccord_malloc(len > 0 ? len : 1u);
  if (!wm->bytes) { ccord_free(wm); return JS_ThrowOutOfMemory(ctx); }
  if (len > 0) memcpy(wm->bytes, bytes, len);
  wm->len = len;
  JSValue obj = JS_NewObjectClass(ctx, v->cls_wasm_mod);
  JS_SetOpaque(obj, wm);
  return obj;
}

static JSValue create_wasm_instance_obj(JSContext *ctx, ccord_web_view *v, const uint8_t *bytes, size_t len,
                                        JSValueConst import_obj)
{
  web_wasm_instance *inst = ccord_calloc(1, sizeof *inst);
  if (!inst) return JS_ThrowOutOfMemory(ctx);
  inst->ctx = ctx;
  inst->bytes = ccord_malloc(len > 0 ? len : 1u);
  if (!inst->bytes) { ccord_free(inst); return JS_ThrowOutOfMemory(ctx); }
  if (len > 0) memcpy(inst->bytes, bytes, len);
  inst->len = len;

  inst->env = m3_NewEnvironment();
  inst->rt = m3_NewRuntime(inst->env, WASM_STACK_BYTES, inst);
  if (!inst->env || !inst->rt) {
    if (inst->rt) m3_FreeRuntime(inst->rt);
    if (inst->env) m3_FreeEnvironment(inst->env);
    ccord_free(inst->bytes);
    ccord_free(inst);
    return JS_ThrowOutOfMemory(ctx);
  }

  web_diag_append(v, "info", "wasm: instantiate (%zu B)", len);
  M3Result r = m3_ParseModule(inst->env, &inst->mod, inst->bytes, (uint32_t)inst->len);
  if (r != m3Err_none) {
    web_diag_append(v, "warn", "wasm: parse error: %s", r);
    m3_FreeRuntime(inst->rt);
    m3_FreeEnvironment(inst->env);
    ccord_free(inst->bytes);
    ccord_free(inst);
    return JS_ThrowTypeError(ctx, "WebAssembly.CompileError: %s", r);
  }

  r = m3_LoadModule(inst->rt, inst->mod);
  if (r != m3Err_none) {
    web_diag_append(v, "warn", "wasm: load error: %s", r);
    // m3_LoadModule transfers module ownership to inst->rt even on failure; m3_FreeRuntime frees it.
    m3_FreeRuntime(inst->rt);
    m3_FreeEnvironment(inst->env);
    ccord_free(inst->bytes);
    ccord_free(inst);
    return JS_ThrowTypeError(ctx, "WebAssembly.LinkError: %s", r);
  }

  // Link JS imported functions dynamically from importObject.
  if (inst->mod->numFuncImports > 0) {
    size_t cap = inst->mod->numFuncImports;
    inst->imports = ccord_calloc(cap, sizeof *inst->imports);
    if (!inst->imports) {
      m3_FreeRuntime(inst->rt);
      m3_FreeEnvironment(inst->env);
      ccord_free(inst->bytes);
      ccord_free(inst);
      return JS_ThrowOutOfMemory(ctx);
    }
    for (uint32_t i = 0; i < inst->mod->numFuncImports; i++) {
      IM3Function fn = &inst->mod->functions[i];
      const char *mname = fn->import.moduleUtf8;
      const char *fname = fn->import.fieldUtf8;
      if (!mname || !fname) continue;
      bool linked = false;
      if (JS_IsObject(import_obj)) {
        JSValue ns = JS_GetPropertyStr(ctx, import_obj, mname);
        if (JS_IsObject(ns)) {
          JSValue jfn = JS_GetPropertyStr(ctx, ns, fname);
          if (JS_IsFunction(ctx, jfn)) {
            web_wasm_import_binding *b = &inst->imports[inst->nimports++];
            b->ctx = ctx;
            b->fn = jfn;
            b->m3_fn = fn;
            char sig[64];
            build_m3_sig(fn, sig);
            M3Result lr = m3_LinkRawFunctionEx(inst->mod, mname, fname, sig, wasm_import_trampoline, b);
            linked = (lr == m3Err_none);
          } else {
            JS_FreeValue(ctx, jfn);
          }
        }
        JS_FreeValue(ctx, ns);
      }
      if (!linked) {
        web_diag_append(v, "warn", "wasm: missing import %s.%s", mname, fname);
      }
    }
  }

  r = m3_CompileModule(inst->mod);
  if (r != m3Err_none) {
    web_diag_append(v, "warn", "wasm: compile error: %s", r);
    for (size_t i = 0; i < inst->nimports; i++) JS_FreeValue(ctx, inst->imports[i].fn);
    ccord_free(inst->imports);
    m3_FreeRuntime(inst->rt);
    m3_FreeEnvironment(inst->env);
    ccord_free(inst->bytes);
    ccord_free(inst);
    return JS_ThrowTypeError(ctx, "WebAssembly.CompileError: %s", r);
  }
  r = m3_RunStart(inst->mod);
  if (r != m3Err_none) {
    web_diag_append(v, "warn", "wasm: start error: %s", r);
  }

  JSValue obj = JS_NewObjectClass(ctx, v->cls_wasm_inst);
  JS_SetOpaque(obj, inst);

  JSValue exports = JS_NewObject(ctx);
  for (uint32_t i = 0; i < inst->mod->numExports; i++) {
    const M3Export *ex = &inst->mod->exports[i];
    if (!ex->name) continue;
    if (ex->kind == d_externalKind_function && ex->index < inst->mod->numFunctions) {
      JSValue fdata[2] = {obj, JS_NewInt32(ctx, (int32_t)ex->index)};
      JSValue fobj = JS_NewCFunctionData(ctx, wasm_export_fn_call, (int)m3_GetArgCount(&inst->mod->functions[ex->index]),
                                         0, 2, fdata);
      JS_FreeValue(ctx, fdata[1]);
      JS_SetPropertyStr(ctx, exports, ex->name, fobj);
    } else if (ex->kind == d_externalKind_memory) {
      JSValue mem_obj = make_wasm_mem_obj(ctx, v, inst->mod, obj, ex->index, 0, 0);
      JS_SetPropertyStr(ctx, exports, ex->name, mem_obj);
    } else if (ex->kind == d_externalKind_global && ex->index < inst->mod->numGlobals) {
      IM3Global g = &inst->mod->globals[ex->index];
      M3TaggedValue tv = {0};
      if (m3_GetGlobal(g, &tv) == m3Err_none) {
        JSValue gobj = JS_NewObject(ctx);
        JSValue gv = JS_UNDEFINED;
        if (tv.type == c_m3Type_i32) gv = JS_NewInt32(ctx, (int32_t)tv.value.i32);
        else if (tv.type == c_m3Type_i64) gv = JS_NewBigInt64(ctx, (int64_t)tv.value.i64);
        else if (tv.type == c_m3Type_f32) gv = JS_NewFloat64(ctx, (double)tv.value.f32);
        else if (tv.type == c_m3Type_f64) gv = JS_NewFloat64(ctx, tv.value.f64);
        JS_SetPropertyStr(ctx, gobj, "value", gv);
        JS_SetPropertyStr(ctx, exports, ex->name, gobj);
      }
    }
  }
  JS_SetPropertyStr(ctx, obj, "exports", exports);
  return obj;
}

static JSValue js_wasm_validate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (argc < 1 || !extract_buffer_bytes(ctx, argv[0], &bytes, &len)) return JS_FALSE;
  IM3Environment env = m3_NewEnvironment();
  IM3Module mod = nullptr;
  M3Result r = m3_ParseModule(env, &mod, bytes, (uint32_t)len);
  if (mod) m3_FreeModule(mod);
  m3_FreeEnvironment(env);
  return JS_NewBool(ctx, r == m3Err_none);
}

static JSValue js_wasm_module_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
  (void)new_target;
  ccord_web_view *v = ctx_view(ctx);
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (!v || argc < 1 || !extract_buffer_bytes(ctx, argv[0], &bytes, &len)) {
    return JS_ThrowTypeError(ctx, "WebAssembly.Module requires an ArrayBuffer or TypedArray");
  }
  return create_wasm_module_obj(ctx, v, bytes, len);
}

static JSValue js_wasm_instance_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
  (void)new_target;
  ccord_web_view *v = ctx_view(ctx);
  if (!v || argc < 1) return JS_ThrowTypeError(ctx, "WebAssembly.Instance requires a Module");
  web_wasm_module *wm = JS_GetOpaque(argv[0], v->cls_wasm_mod);
  if (!wm) return JS_ThrowTypeError(ctx, "First argument must be a WebAssembly.Module");
  JSValueConst imp = argc > 1 ? argv[1] : JS_UNDEFINED;
  return create_wasm_instance_obj(ctx, v, wm->bytes, wm->len, imp);
}

static JSValue make_resolved_or_rejected(JSContext *ctx, JSValue val)
{
  JSValue funcs[2];
  JSValue p = JS_NewPromiseCapability(ctx, funcs);
  if (JS_IsException(val)) {
    JSValue ex = JS_GetException(ctx);
    JSValue r = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &ex);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, ex);
  } else {
    JSValue r = JS_Call(ctx, funcs[0], JS_UNDEFINED, 1, &val);
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, val);
  }
  JS_FreeValue(ctx, funcs[0]);
  JS_FreeValue(ctx, funcs[1]);
  return p;
}

static JSValue js_wasm_compile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  ccord_web_view *v = ctx_view(ctx);
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (!v || argc < 1 || !extract_buffer_bytes(ctx, argv[0], &bytes, &len)) {
    return make_resolved_or_rejected(ctx, JS_ThrowTypeError(ctx, "WebAssembly.compile requires bytes"));
  }
  return make_resolved_or_rejected(ctx, create_wasm_module_obj(ctx, v, bytes, len));
}

static JSValue js_wasm_instantiate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
  (void)this_val;
  ccord_web_view *v = ctx_view(ctx);
  if (!v || argc < 1) return make_resolved_or_rejected(ctx, JS_ThrowTypeError(ctx, "Missing argument"));
  JSValueConst imp = argc > 1 ? argv[1] : JS_UNDEFINED;
  web_wasm_module *wm = JS_GetOpaque(argv[0], v->cls_wasm_mod);
  if (wm) {
    JSValue inst = create_wasm_instance_obj(ctx, v, wm->bytes, wm->len, imp);
    return make_resolved_or_rejected(ctx, inst);
  }
  const uint8_t *bytes = nullptr;
  size_t len = 0;
  if (!extract_buffer_bytes(ctx, argv[0], &bytes, &len)) {
    return make_resolved_or_rejected(ctx, JS_ThrowTypeError(ctx, "Expected Module or BufferSource"));
  }
  JSValue mod = create_wasm_module_obj(ctx, v, bytes, len);
  if (JS_IsException(mod)) return make_resolved_or_rejected(ctx, mod);
  JSValue inst = create_wasm_instance_obj(ctx, v, bytes, len, imp);
  if (JS_IsException(inst)) {
    JS_FreeValue(ctx, mod);
    return make_resolved_or_rejected(ctx, inst);
  }
  JSValue out = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, out, "module", mod);
  JS_SetPropertyStr(ctx, out, "instance", inst);
  return make_resolved_or_rejected(ctx, out);
}

void web_wasm_register_classes(ccord_web_view *v)
{
  JS_NewClassID(v->rt, &v->cls_wasm_mod);
  JS_NewClassID(v->rt, &v->cls_wasm_inst);
  JS_NewClassID(v->rt, &v->cls_wasm_mem);
  JSClassDef md = {.class_name = "WebAssembly.Module", .finalizer = wasm_mod_finalizer};
  JSClassDef id = {.class_name = "WebAssembly.Instance", .finalizer = wasm_inst_finalizer, .gc_mark = wasm_inst_gc_mark};
  JSClassDef memd = {.class_name = "WebAssembly.Memory", .finalizer = wasm_mem_finalizer, .gc_mark = wasm_mem_gc_mark};
  JS_NewClass(v->rt, v->cls_wasm_mod, &md);
  JS_NewClass(v->rt, v->cls_wasm_inst, &id);
  JS_NewClass(v->rt, v->cls_wasm_mem, &memd);
}

void web_wasm_install(web_frame *f)
{
  JSContext *ctx = f->ctx;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue wasm = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, wasm, "validate", JS_NewCFunction(ctx, js_wasm_validate, "validate", 1));
  JS_SetPropertyStr(ctx, wasm, "compile", JS_NewCFunction(ctx, js_wasm_compile, "compile", 1));
  JS_SetPropertyStr(ctx, wasm, "instantiate", JS_NewCFunction(ctx, js_wasm_instantiate, "instantiate", 2));
  JS_SetPropertyStr(ctx, wasm, "Module",
                    JS_NewCFunction2(ctx, js_wasm_module_ctor, "Module", 1, JS_CFUNC_constructor, 0));
  JS_SetPropertyStr(ctx, wasm, "Instance",
                    JS_NewCFunction2(ctx, js_wasm_instance_ctor, "Instance", 2, JS_CFUNC_constructor, 0));
  JS_SetPropertyStr(ctx, wasm, "Memory",
                    JS_NewCFunction2(ctx, js_wasm_memory_ctor, "Memory", 1, JS_CFUNC_constructor, 0));
  JS_SetPropertyStr(ctx, global, "WebAssembly", wasm);
  JS_FreeValue(ctx, global);

  // Add instantiateStreaming, compileStreaming, Table, Global, and Error subclasses cleanly in JS.
  static const char k_wasm_extra[] =
    "WebAssembly.instantiateStreaming = function(respP, imp) {"
    "  return Promise.resolve(respP).then(r => r.arrayBuffer()).then(b => WebAssembly.instantiate(b, imp));"
    "};"
    "WebAssembly.compileStreaming = function(respP) {"
    "  return Promise.resolve(respP).then(r => r.arrayBuffer()).then(b => WebAssembly.compile(b));"
    "};"
    "WebAssembly.CompileError = class CompileError extends Error { constructor(m){ super(m); this.name='CompileError'; } };"
    "WebAssembly.LinkError = class LinkError extends Error { constructor(m){ super(m); this.name='LinkError'; } };"
    "WebAssembly.RuntimeError = class RuntimeError extends Error { constructor(m){ super(m); this.name='RuntimeError'; } };"
    "WebAssembly.Table = class Table { constructor(d){ this._a = new Array(d && d.initial || 0).fill(null); }"
    "  get length(){ return this._a.length; } get(i){ return this._a[i]; } set(i,v){ this._a[i]=v; }"
    "  grow(n){ const o=this._a.length; for(let i=0;i<n;i++) this._a.push(null); return o; } };"
    "WebAssembly.Global = class Global { constructor(d,v){ this.value = v !== undefined ? v : 0; } valueOf(){ return this.value; } };";
  JSValue rv = JS_Eval(ctx, k_wasm_extra, sizeof k_wasm_extra - 1u, "<wasm-init>", JS_EVAL_TYPE_GLOBAL);
  JS_FreeValue(ctx, rv);
}
