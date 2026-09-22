/*
 * js_wasm.c - WebAssembly natives for the mario JavaScript VM.
 *
 * The guest runtime is the embedded easm engine (easm/ in the repo root).
 * This bridge owns a per-VM EaStore (released with the VM) and translates
 * between mario vars and easm WVal / EaModule / EaInstance.
 *
 * Lifetime model
 *   - The EaModule behind a WebAssembly.Module is refcounted (WasmModRef).
 *     The Module wrapper and every Instance over it each hold a ref, so a
 *     Module collected before its Instance still keeps the bytecode + type
 *     space (which funcs point into) alive.
 *   - Import values handed through the JS import object (funcs, Memory /
 *     Table / Global wrappers) are anchored in a hidden "@@imports" array on
 *     the Instance var, so they stay alive for as long as the instance does.
 *   - Imported Memory / Table / Global and exported functions/objects borrow
 *     raw easm storage from the owning instance. They are valid only while
 *     the Instance that produced them is reachable. (Documented limitation.)
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "js_wasm.h"
#include "js_natives_priv.h"
#include "../../easm/src/easm.h"
#include "../../easm/src/wasi.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WASM_BRIDGE_KEY  "@@wasm_bridge"
#define WASM_KIND_KEY    "@@wasmkind"
#define WASM_EXPORTS_KEY "@@exports"
#define WASM_IMPORTS_KEY "@@imports"

#define WASM_CLS_MODULE   "Module"
#define WASM_CLS_INSTANCE "Instance"
#define WASM_CLS_MEMORY   "Memory"
#define WASM_CLS_TABLE    "Table"
#define WASM_CLS_GLOBAL   "Global"
#define WASM_CLS_CE       "CompileError"
#define WASM_CLS_LE       "LinkError"
#define WASM_CLS_RE       "RuntimeError"

/* ------------------------------------------------------------------ */
/* Per-VM bridge state                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    vm_t*    vm;
    EaStore* store;
    WasiCtx* wasi;                 /* lazily created on first wasi_snapshot import */
    bool     wasi_init_failed;
} WasmState;

static void wasm_state_free(void* p) {
    WasmState* st = (WasmState*)p;
    if(st == NULL) return;
    if(st->wasi != NULL) ea_wasi_ctx_free(st->wasi);
    if(st->store != NULL) ea_store_free(st->store);
    mario_free(st);
}

static WasmState* wasm_state(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return NULL;
    var_t* b = var_find_own_member_var(vm->root, WASM_BRIDGE_KEY);
    return (b != NULL) ? (WasmState*)b->value : NULL;
}

static bool wasm_ensure_wasi(vm_t* vm, WasmState* st) {
    (void)vm;
    if(st->wasi != NULL) return true;
    if(st->wasi_init_failed) return false;
    WasiCtx* ctx = ea_wasi_ctx_new(NULL, 0, NULL, 0);
    char* err = NULL;
    if(ctx == NULL || ea_wasi_init(st->store, ctx, &err) != 0) {
        if(ctx != NULL) ea_wasi_ctx_free(ctx);
        if(err != NULL) free(err);
        st->wasi_init_failed = true;
        return false;
    }
    st->wasi = ctx;
    return true;
}

/* ------------------------------------------------------------------ */
/* Generic helpers                                                   */
/* ------------------------------------------------------------------ */

static char* wasm_strdup(const char* s) {
    if(s == NULL) return NULL;
    size_t n = strlen(s);
    char* p = (char*)mario_malloc((uint32_t)n + 1);
    if(p == NULL) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static void wasm_mark(var_t* v, const char* kind) {
    if(v == NULL) return;
    node_t* n = var_add(v, WASM_KIND_KEY, var_new_str(v->vm, kind));
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
}

static bool wasm_is_kind(var_t* v, const char* kind) {
    if(v == NULL) return false;
    var_t* k = var_find_own_member_var(v, WASM_KIND_KEY);
    if(k == NULL || k->type != V_STRING) return false;
    const char* s = var_get_str(k);
    return s != NULL && strcmp(s, kind) == 0;
}

static var_t* wasm_class_proto(vm_t* vm, const char* clsname) {
    node_t* cn = vm_load_node(vm, clsname, false);
    var_t* cls = (cn != NULL) ? cn->var : NULL;
    return (cls != NULL) ? var_get_prototype(cls) : NULL;
}

static var_t* wasm_bare_obj(vm_t* vm, const char* clsname) {
    var_t* proto = (clsname != NULL) ? wasm_class_proto(vm, clsname) : NULL;
    return var_new_obj(vm, proto, NULL, NULL);
}

/* Extract a byte range from an ArrayBuffer / TypedArray / DataView. */
static bool wasm_bytes_of(var_t* v, const uint8_t** out, uint32_t* out_len) {
    if(out != NULL) *out = NULL;
    if(out_len != NULL) *out_len = 0;
    if(v == NULL || v->type != V_OBJECT) return false;
    if(var_is_arraybuffer(v)) {
        if(out != NULL) *out = (const uint8_t*)v->value;
        if(out_len != NULL) *out_len = v->size;
        return true;
    }
    if(var_is_typedarray(v) || var_is_dataview(v)) {
        var_t* buf = get_obj(v, "buffer");
        if(buf == NULL || !var_is_arraybuffer(buf)) return false;
        int64_t off = 0, len = 0;
        var_t* ov = get_obj(v, "byteOffset");
        var_t* lv = get_obj(v, "byteLength");
        if(ov != NULL && ov->type != V_UNDEF) off = var_get_int64(ov);
        if(lv != NULL && lv->type != V_UNDEF) len = var_get_int64(lv);
        if(off < 0 || len < 0) return false;
        if(out != NULL) *out = (const uint8_t*)buf->value + off;
        if(out_len != NULL) *out_len = (uint32_t)len;
        return true;
    }
    return false;
}

static void wasm_noop_free(void* p) { (void)p; }

/* New ArrayBuffer aliasing `base`/`len` (wasm linear memory bytes). */
static var_t* wasm_ab_alias(vm_t* vm, uint8_t* base, uint32_t len) {
    var_t* cls = wasm_class_proto(vm, "ArrayBuffer");
    var_t* b = (cls != NULL) ? var_new_obj(vm, cls, base, wasm_noop_free)
                             : var_new_obj_no_proto(vm, base, wasm_noop_free);
    if(b == NULL) return NULL;
    b->size = len;
    node_t* mn = var_add(b, EXOTIC_MARKER, var_new_str(vm, EXOTIC_ARRAYBUFFER));
    if(mn != NULL) { mn->invisable = 1; mn->be_unenumerable = 1; }
    node_t* bn = var_add(b, "byteLength", var_new_int(vm, (int)len));
    if(bn != NULL) bn->be_unenumerable = 1;
    return b;
}

static int64_t wasm_to_i64(var_t* v) {
    if(v == NULL) return 0;
    if(v->type == V_BIGINT) {
        bignum_t* bn = var_get_bigint(v);
        return (bn != NULL) ? bn_to_int64(bn) : 0;
    }
    return (int64_t)var_get_float64(v);
}

static EaValType wasm_typestr(const char* s) {
    if(s == NULL) return VT_BOTTOM;
    if(strcmp(s, "i32") == 0) return VT_I32;
    if(strcmp(s, "i64") == 0) return VT_I64;
    if(strcmp(s, "f32") == 0) return VT_F32;
    if(strcmp(s, "f64") == 0) return VT_F64;
    if(strcmp(s, "anyfunc") == 0 || strcmp(s, "funcref") == 0) return VT_FUNCREF;
    if(strcmp(s, "externref") == 0) return VT_EXTERNREF;
    return VT_BOTTOM;
}

static void wasm_js_to_val(vm_t* vm, var_t* v, EaValType t, WVal* out) {
    (void)vm;
    memset(out, 0, sizeof(*out));
    switch(t) {
    case VT_I32: out->i32 = (uint32_t)(int32_t)(int64_t)var_get_float64(v); break;
    case VT_I64: out->i64 = (uint64_t)wasm_to_i64(v); break;
    case VT_F32: out->f32 = (float)var_get_float64(v); break;
    case VT_F64: out->f64 = var_get_float64(v); break;
    default:
        out->ref = (v != NULL && v->type != V_NULL && v->type != V_UNDEF)
                       ? (void*)((uintptr_t)v + 1) : NULL;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Value conversion back to JS                                       */
/* ------------------------------------------------------------------ */

typedef struct WasmFuncData WasmFuncData;
static var_t* wasm_fn_wrap(vm_t* vm, EaFuncInst* fi);

static var_t* wasm_val_to_js(vm_t* vm, const WVal* v, EaValType t) {
    switch(t) {
    case VT_I32: return var_new_int(vm, (int)v->i32);
    case VT_I64: return var_new_float64(vm, (double)v->i64);
    case VT_F32: return var_new_float(vm, v->f32);
    case VT_F64: return var_new_float64(vm, v->f64);
    case VT_FUNCREF:
        if(v->ref == NULL) return var_new_null(vm);
        if(ea_is_i31(v->ref) || ea_gc_is_heap(v->ref))
            return var_new_obj_no_proto(vm, NULL, NULL);   /* opaque wasm ref */
        return wasm_fn_wrap(vm, (EaFuncInst*)v->ref);
    case VT_EXTERNREF:
        if(v->ref == NULL) return var_new_null(vm);
        if(ea_is_i31(v->ref) || ea_gc_is_heap(v->ref))
            return var_new_obj_no_proto(vm, NULL, NULL);
        return (var_t*)((uintptr_t)v->ref - 1);
    default:
        if(v->ref == NULL) return var_new_null(vm);
        if(ea_is_i31(v->ref) || ea_gc_is_heap(v->ref))
            return var_new_obj_no_proto(vm, NULL, NULL);
        return var_new_obj_no_proto(vm, NULL, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Exported function wrappers                                        */
/* ------------------------------------------------------------------ */

struct WasmFuncData {
    EaStore*    store;
    EaInstance* inst;
    uint32_t    func_idx;
    EaFuncType* type;
};

static void wasm_func_destroy(var_t* v) {
    if(v == NULL) return;
    func_t* f = (v->is_func) ? var_get_func(v) : NULL;
    if(f == NULL) return;
    WasmFuncData* d = (WasmFuncData*)f->data;
    f->data = NULL;
    if(d != NULL) mario_free(d);
    v->on_destroy = NULL;
}

static var_t* wasm_exported_call(vm_t* vm, var_t* env, void* data) {
    WasmFuncData* d = (WasmFuncData*)data;
    if(d == NULL || d->store == NULL || d->inst == NULL) return var_new_null(vm);
    EaFuncType* ft = d->type;
    uint32_t np = (ft != NULL) ? ft->n_params : 0;
    uint32_t nr = (ft != NULL) ? ft->n_results : 0;
    WVal args[64];
    memset(args, 0, sizeof(args));
    for(uint32_t i = 0; i < np && i < 64; i++)
        wasm_js_to_val(vm, js_arg(env, (int)i), ft->params[i], &args[i]);
    WVal results[64];
    memset(results, 0, sizeof(results));
    EaTrap trap = TRAP_NONE;
    int rc = ea_instance_invoke(d->store, d->inst, d->func_idx, args, results, &trap);
    if(rc != 0) {
        if(vm != NULL && vm->propagating_err != NULL)
            return NULL;   /* a host JS import threw; keep it propagating */
        vm_throw_type_native(vm, WASM_CLS_RE, "%s",
                             ea_trap_msg(trap != TRAP_NONE ? trap : TRAP_HOST));
        return var_new_null(vm);
    }
    if(nr == 0) return var_new(vm);
    return wasm_val_to_js(vm, &results[0], ft->results[0]);
}

static var_t* wasm_fn_wrap(vm_t* vm, EaFuncInst* fi) {
    if(fi == NULL || fi->is_host) return var_new_null(vm);
    WasmFuncData* d = (WasmFuncData*)mario_malloc(sizeof(WasmFuncData));
    if(d == NULL) return var_new_null(vm);
    d->store = (fi->inst != NULL) ? fi->inst->store : NULL;
    d->inst = fi->inst;
    d->func_idx = fi->func_idx;
    d->type = fi->type;
    var_t* f = var_new_native_func(vm, wasm_exported_call, d);
    if(f == NULL) { mario_free(d); return var_new_null(vm); }
    f->on_destroy = wasm_func_destroy;
    wasm_mark(f, "func");
    return f;
}

/* ------------------------------------------------------------------ */
/* Compilation + module object                                       */
/* ------------------------------------------------------------------ */

static bool wasm_compile_module(vm_t* vm, WasmState* st, const uint8_t* bytes,
                                uint32_t len, EaModule** out) {
    *out = NULL;
    if(st == NULL || st->store == NULL) return false;
    char* err = NULL;
    EaModule* m = NULL;
    if(ea_store_load(st->store, bytes, len, &m, &err) != 0) {
        vm_throw_type_native(vm, WASM_CLS_CE, "Compiling module failed: %s",
                             (err != NULL) ? err : "malformed bytes");
        if(err != NULL) free(err);
        return false;
    }
    if(err != NULL) free(err);
    if(getenv("EA_NO_JIT") == NULL)
        ea_jit_compile_module(m);
    *out = m;
    return true;
}

/* Refcounted EaModule: the Module wrapper and every Instance over it each
 * hold a ref, so a Module collected before its Instance still keeps the
 * type/bytecode space alive. */
typedef struct {
    EaModule* mod;
    uint32_t  refs;
} WasmModRef;

static WasmModRef* wasm_modref_new(EaModule* mod) {
    WasmModRef* r = (WasmModRef*)mario_malloc(sizeof(WasmModRef));
    if(r == NULL) return NULL;
    r->mod = mod;
    r->refs = 1;
    return r;
}

static void wasm_modref_free(void* p) {
    WasmModRef* r = (WasmModRef*)p;
    if(r == NULL) return;
    if(--r->refs == 0) {
        if(r->mod != NULL) ea_module_free(r->mod);
        mario_free(r);
    }
}

static const char* wasm_kind_name(uint8_t kind) {
    switch(kind) {
    case EAK_FUNC:   return "function";
    case EAK_TABLE:  return "table";
    case EAK_MEMORY: return "memory";
    case EAK_GLOBAL: return "global";
    case EAK_TAG:    return "tag";
    default:         return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Module class natives                                              */
/* ------------------------------------------------------------------ */

static var_t* wasm_module_obj(vm_t* vm, EaModule* mod) {
    var_t* mv = wasm_bare_obj(vm, WASM_CLS_MODULE);
    if(mv == NULL) { ea_module_free(mod); return NULL; }
    WasmModRef* r = wasm_modref_new(mod);
    if(r == NULL) { ea_module_free(mod); return NULL; }
    mv->value = r;
    mv->free_func = wasm_modref_free;
    wasm_mark(mv, "module");
    return mv;
}

static var_t* native_Module_constructor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* this_v = js_this(env);
    const uint8_t* bytes = NULL;
    uint32_t len = 0;
    if(!wasm_bytes_of(js_arg(env, 0), &bytes, &len)) {
        vm_throw_type_native(vm, "TypeError",
                             "WebAssembly.Module requires a buffer source");
        return this_v;
    }
    WasmState* st = wasm_state(vm);
    EaModule* m = NULL;
    if(!wasm_compile_module(vm, st, bytes, len, &m)) return this_v;
    WasmModRef* r = wasm_modref_new(m);
    if(r == NULL) { ea_module_free(m); return this_v; }
    this_v->value = r;
    this_v->free_func = wasm_modref_free;
    wasm_mark(this_v, "module");
    return this_v;
}

static var_t* native_Module_get_exports(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmModRef* r = (self != NULL) ? (WasmModRef*)self->value : NULL;
    var_t* arr = var_new_array(vm);
    if(r == NULL || r->mod == NULL) return arr;
    EaModule* m = r->mod;
    for(uint32_t i = 0; i < m->n_exports; i++) {
        EaExport* ex = &m->exports[i];
        var_t* o = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(o, "name", var_new_str(vm, ex->name));
        var_add(o, "kind", var_new_str(vm, wasm_kind_name(ex->kind)));
        var_array_add(arr, o);
    }
    return arr;
}

static var_t* native_Module_get_imports(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmModRef* r = (self != NULL) ? (WasmModRef*)self->value : NULL;
    var_t* arr = var_new_array(vm);
    if(r == NULL || r->mod == NULL) return arr;
    EaModule* m = r->mod;
    for(uint32_t i = 0; i < m->n_imports; i++) {
        EaImport* in = &m->imports[i];
        var_t* o = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(o, "module", var_new_str(vm, in->module));
        var_add(o, "name", var_new_str(vm, in->name));
        var_add(o, "kind", var_new_str(vm, wasm_kind_name(in->kind)));
        var_array_add(arr, o);
    }
    return arr;
}

static var_t* native_Module_customSections(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return var_new_array(vm);
}

/* ------------------------------------------------------------------ */
/* Memory                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    EaMemInst* mi;
    bool       owned;
} WasmMemData;

static void wasm_mem_free(void* p) {
    WasmMemData* d = (WasmMemData*)p;
    if(d == NULL) return;
    if(d->owned) ea_memory_free(d->mi);
    mario_free(d);
}

static var_t* native_Memory_constructor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* this_v = js_this(env);
    var_t* desc = js_arg(env, 0);
    if(desc == NULL || desc->type != V_OBJECT) {
        vm_throw_type_native(vm, "TypeError", "Memory descriptor is required");
        return this_v;
    }
    var_t* iv = get_obj(desc, "initial");
    var_t* mv = get_obj(desc, "maximum");
    int64_t initial = (iv != NULL && iv->type != V_UNDEF) ? (int64_t)var_get_float64(iv) : 0;
    int64_t maximum = (mv != NULL && mv->type != V_UNDEF) ? (int64_t)var_get_float64(mv) : 65536;
    bool has_max = (mv != NULL && mv->type != V_UNDEF);
    if(initial < 0 || initial > 65536 || (has_max && (maximum < initial || maximum > 65536))) {
        vm_throw_type_native(vm, WASM_CLS_RE,
                             "Memory page counts must be within [0, 65536]");
        return this_v;
    }
    EaMemInst* mi = ea_memory_new((uint64_t)initial, (uint64_t)maximum, has_max, false);
    if(mi == NULL) {
        vm_throw_type_native(vm, WASM_CLS_RE, "Unable to allocate memory");
        return this_v;
    }
    WasmMemData* d = (WasmMemData*)mario_malloc(sizeof(WasmMemData));
    if(d != NULL) {
        d->mi = mi; d->owned = true;
        this_v->value = d;
        this_v->free_func = wasm_mem_free;
    }
    wasm_mark(this_v, "memory");
    return this_v;
}

static var_t* native_Memory_get_buffer(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmMemData* d = (self != NULL) ? (WasmMemData*)self->value : NULL;
    if(d == NULL || d->mi == NULL) return wasm_ab_alias(vm, NULL, 0);
    return wasm_ab_alias(vm, d->mi->base, (uint32_t)d->mi->size);
}

static var_t* native_Memory_grow(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmMemData* d = (self != NULL) ? (WasmMemData*)self->value : NULL;
    if(d == NULL || d->mi == NULL) return var_new_int(vm, -1);
    int64_t delta = (int64_t)var_get_float64(js_arg(env, 0));
    uint64_t old = 0;
    if(delta < 0 || !ea_grow_memory(d->mi, (uint64_t)delta, &old)) {
        vm_throw_type_native(vm, WASM_CLS_RE, "Unable to grow instance memory");
        return var_new_int(vm, 0);
    }
    return var_new_int(vm, (int)old);
}

/* ------------------------------------------------------------------ */
/* Table                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    EaTableInst* ti;
    bool         owned;
} WasmTableData;

static void wasm_table_free(void* p) {
    WasmTableData* d = (WasmTableData*)p;
    if(d == NULL) return;
    if(d->owned) ea_table_free(d->ti);
    mario_free(d);
}

static var_t* native_Table_Constructor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* this_v = js_this(env);
    var_t* desc = js_arg(env, 0);
    if(desc == NULL || desc->type != V_OBJECT) {
        vm_throw_type_native(vm, "TypeError", "Table descriptor is required");
        return this_v;
    }
    EaValType rt = wasm_typestr(get_str(desc, "element"));
    if(rt != VT_FUNCREF && rt != VT_EXTERNREF) {
        vm_throw_type_native(vm, WASM_CLS_CE, "Invalid table element type");
        return this_v;
    }
    int64_t initial = (int64_t)var_get_float64(get_obj(desc, "initial"));
    var_t* mv = get_obj(desc, "maximum");
    bool has_max = (mv != NULL && mv->type != V_UNDEF);
    int64_t maximum = has_max ? (int64_t)var_get_float64(mv) : 0;
    if(initial < 0 || (has_max && (maximum < initial || maximum > 1000000000))) {
        vm_throw_type_native(vm, WASM_CLS_RE, "Invalid table size");
        return this_v;
    }
    EaTableInst* ti = ea_table_new(rt, (uint64_t)initial,
                                   has_max ? (uint64_t)maximum : UINT64_MAX, has_max, false);
    WasmTableData* d = (WasmTableData*)mario_malloc(sizeof(WasmTableData));
    if(d != NULL) {
        d->ti = ti; d->owned = true;
        this_v->value = d;
        this_v->free_func = wasm_table_free;
    }
    wasm_mark(this_v, "table");
    return this_v;
}

static var_t* native_Table_get_length(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmTableData* d = (self != NULL) ? (WasmTableData*)self->value : NULL;
    if(d == NULL || d->ti == NULL) return var_new_int(vm, 0);
    return var_new_int(vm, (int)d->ti->size);
}

static var_t* native_Table_get(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmTableData* d = (self != NULL) ? (WasmTableData*)self->value : NULL;
    if(d == NULL || d->ti == NULL) return var_new_null(vm);
    int64_t i = (int64_t)var_get_float64(js_arg(env, 0));
    if(i < 0 || (uint64_t)i >= d->ti->size) {
        vm_throw_type_native(vm, WASM_CLS_RE, "Table index out of bounds");
        return var_new_null(vm);
    }
    return wasm_val_to_js(vm, &d->ti->elems[i], d->ti->ref_type);
}

static var_t* native_Table_set(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmTableData* d = (self != NULL) ? (WasmTableData*)self->value : NULL;
    if(d == NULL || d->ti == NULL) return var_new_null(vm);
    int64_t i = (int64_t)var_get_float64(js_arg(env, 0));
    if(i < 0 || (uint64_t)i >= d->ti->size) {
        vm_throw_type_native(vm, WASM_CLS_RE, "Table index out of bounds");
        return var_new_null(vm);
    }
    WVal wv;
    memset(&wv, 0, sizeof(wv));
    if(d->ti->ref_type == VT_FUNCREF) {
        var_t* v = js_arg(env, 1);
        if(v == NULL || v->type == V_NULL || v->type == V_UNDEF) wv.ref = NULL;
        else if(wasm_is_kind(v, "func")) wv.ref = (void*)v->value;
        else { vm_throw_type_native(vm, "TypeError", "Invalid funcref value"); return var_new_null(vm); }
    } else {
        wasm_js_to_val(vm, js_arg(env, 1), d->ti->ref_type, &wv);
    }
    d->ti->elems[i] = wv;
    return var_new_null(vm);
}

static var_t* native_Table_grow(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmTableData* d = (self != NULL) ? (WasmTableData*)self->value : NULL;
    if(d == NULL || d->ti == NULL) return var_new_int(vm, -1);
    int64_t delta = (int64_t)var_get_float64(js_arg(env, 0));
    if(delta < 0) { vm_throw_type_native(vm, WASM_CLS_RE, "Negative table growth"); return var_new_int(vm, -1); }
    uint64_t old = d->ti->size;
    uint64_t neu = old + (uint64_t)delta;
    if(neu > d->ti->max || neu > (1ull << 32)) {
        vm_throw_type_native(vm, WASM_CLS_RE, "Unable to grow instance table");
        return var_new_int(vm, -1);
    }
    WVal iv;
    memset(&iv, 0, sizeof(iv));
    if(d->ti->ref_type != VT_FUNCREF && d->ti->ref_type != VT_EXTERNREF)
        wasm_js_to_val(vm, js_arg(env, 1), d->ti->ref_type, &iv);
    WVal* ne = (WVal*)ea_realloc(d->ti->elems, (size_t)(neu > 0 ? neu : 1) * sizeof(WVal));
    if(ne == NULL) { vm_throw_type_native(vm, WASM_CLS_RE, "Failed to grow table"); return var_new_int(vm, -1); }
    for(uint64_t k = old; k < neu; k++) ne[k] = iv;
    d->ti->elems = ne;
    d->ti->size = neu;
    return var_new_int(vm, (int)old);
}

/* ------------------------------------------------------------------ */
/* Global                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    WVal*     slot;
    EaValType type;
    bool      mut;
    bool      owned;
} WasmGlobalData;

static void wasm_global_free(void* p) {
    WasmGlobalData* d = (WasmGlobalData*)p;
    if(d == NULL) return;
    if(d->owned && d->slot != NULL) mario_free(d->slot);
    mario_free(d);
}

static var_t* native_Global_Constructor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* this_v = js_this(env);
    var_t* desc = js_arg(env, 0);
    if(desc == NULL || desc->type != V_OBJECT) {
        vm_throw_type_native(vm, "TypeError", "Global descriptor is required");
        return this_v;
    }
    EaValType t = wasm_typestr(get_str(desc, "value"));
    if(t == VT_BOTTOM || t == VT_FUNCREF || t == VT_EXTERNREF) {
        vm_throw_type_native(vm, WASM_CLS_RE, "Invalid global value type");
        return this_v;
    }
    bool mut = get_bool(desc, "mutable");
    WVal* slot = (WVal*)mario_malloc(sizeof(WVal));
    if(slot != NULL) {
        memset(slot, 0, sizeof(WVal));
        wasm_js_to_val(vm, js_arg(env, 1), t, slot);
        WasmGlobalData* d = (WasmGlobalData*)mario_malloc(sizeof(WasmGlobalData));
        if(d != NULL) {
            d->slot = slot; d->type = t; d->mut = mut; d->owned = true;
            this_v->value = d;
            this_v->free_func = wasm_global_free;
        }
    }
    wasm_mark(this_v, "global");
    return this_v;
}

static var_t* native_Global_get_value(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmGlobalData* d = (self != NULL) ? (WasmGlobalData*)self->value : NULL;
    if(d == NULL || d->slot == NULL) return var_new_null(vm);
    return wasm_val_to_js(vm, d->slot, d->type);
}

static var_t* native_Global_set_value(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    WasmGlobalData* d = (self != NULL) ? (WasmGlobalData*)self->value : NULL;
    if(d == NULL || d->slot == NULL) return var_new_null(vm);
    if(!d->mut) {
        vm_throw_type_native(vm, "TypeError", "Immutable global cannot be assigned");
        return var_new_null(vm);
    }
    wasm_js_to_val(vm, get_func_arg(env, 0), d->type, d->slot);
    return var_new_null(vm);
}

static var_t* native_Global_valueOf(vm_t* vm, var_t* env, void* data) {
    return native_Global_get_value(vm, env, data);
}

/* ------------------------------------------------------------------ */
/* Host (JS) import call bridge                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    vm_t*       vm;
    var_t*      fn;       /* rooted by the instance's @@imports array */
    EaFuncType* type;
} WasmImportSlot;

static int wasm_import_call(void* user, const WVal* args, WVal* results) {
    WasmImportSlot* slot = (WasmImportSlot*)user;
    if(slot == NULL || slot->vm == NULL || slot->fn == NULL) return 1;
    vm_t* vm = slot->vm;
    EaFuncType* ft = slot->type;
    uint32_t np = (ft != NULL) ? ft->n_params : 0;
    uint32_t nr = (ft != NULL) ? ft->n_results : 0;
    if(np > 32) return 1;

    var_t* args_arr = var_new_array(vm);
    for(uint32_t i = 0; i < np; i++) {
        EaValType pt = ft->params[i];
        if(pt == VT_I32) var_array_add(args_arr, var_new_int(vm, (int)args[i].i32));
        else if(pt == VT_I64) var_array_add(args_arr, var_new_int64(vm, (int64_t)args[i].i64));
        else if(pt == VT_F32) var_array_add(args_arr, var_new_float(vm, args[i].f32));
        else if(pt == VT_F64) var_array_add(args_arr, var_new_float64(vm, args[i].f64));
        else if(pt == VT_EXTERNREF) {
            if(args[i].ref == NULL) var_array_add(args_arr, var_new_null(vm));
            else if(ea_is_i31(args[i].ref) || ea_gc_is_heap(args[i].ref))
                var_array_add(args_arr, var_new_obj_no_proto(vm, NULL, NULL));
            else var_array_add(args_arr, (var_t*)((uintptr_t)args[i].ref - 1));
        }
        else if(pt == VT_FUNCREF) {
            if(args[i].ref == NULL) var_array_add(args_arr, var_new_null(vm));
            else if(ea_is_i31(args[i].ref) || ea_gc_is_heap(args[i].ref))
                var_array_add(args_arr, var_new_obj_no_proto(vm, NULL, NULL));
            else var_array_add(args_arr, wasm_fn_wrap(vm, (EaFuncInst*)args[i].ref));
        } else {
            var_array_add(args_arr, var_new_null(vm));
        }
    }
    var_t* ret = call_m_func(vm, NULL, slot->fn, args_arr);
    var_unref(args_arr);
    if(ret == NULL) return 1;   /* host JS threw: surface as TRAP_HOST */

    for(uint32_t i = 0; i < nr; i++) {
        EaValType rt = ft->results[i];
        if(rt == VT_I32) results[i].i32 = (uint32_t)var_get_int64(ret);
        else if(rt == VT_I64) results[i].i64 = (uint64_t)wasm_to_i64(ret);
        else if(rt == VT_F32) results[i].f32 = (float)var_get_float64(ret);
        else if(rt == VT_F64) results[i].f64 = var_get_float64(ret);
        else results[i].ref = NULL;
    }
    var_unref(ret);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Instance internals                                                */
/* ------------------------------------------------------------------ */

/* One synthetic module per imported namespace, registered in the store only
 * for the duration of ea_store_instantiate so guest imports resolve by name. */
typedef struct WasmImport {
    char*         name;
    var_t*        ns_obj;         /* rooted in @@imports */
    EaModule      mod;
    EaExport*     exports;
    EaFuncType*   ftypes;
    EaValType*    type_bufs;
    EaFuncInst*   funcs;
    EaTableInst*  tables;
    EaMemInst**   mems;
    WVal**        globals;
    EaGlobal*     globals_def;
    WasmImportSlot* slots;
    EaInstance*   registered;     /* the synthetic EaInstance we register */
    uint32_t      n_funcs, n_tables, n_mems, n_globals, n_exports, n_buf;
    struct WasmImport* next;
} WasmImport;

typedef struct {
    EaStore*    store;
    EaInstance* inst;
    WasmModRef* modref;
    WasmImport* imports;
    bool        wasi_registered;
} WasmInstance;

static void wasm_import_free(WasmImport* im) {
    if(im == NULL) return;
    for(uint32_t i = 0; i < im->n_exports; i++)
        mario_free(im->exports[i].name);
    mario_free(im->exports);
    mario_free(im->ftypes);
    mario_free(im->type_bufs);
    mario_free(im->funcs);
    mario_free(im->tables);
    mario_free(im->mems);
    mario_free(im->globals);
    mario_free(im->globals_def);
    mario_free(im->slots);
    if(im->registered != NULL) mario_free(im->registered);
    mario_free(im->name);
    mario_free(im);
}

static void wasm_instance_free(void* p) {
    WasmInstance* w = (WasmInstance*)p;
    if(w == NULL) return;
    if(w->inst != NULL) ea_instance_free(w->inst);
    WasmImport* i = w->imports;
    while(i != NULL) {
        WasmImport* nx = i->next;
        wasm_import_free(i);
        i = nx;
    }
    if(w->modref != NULL) wasm_modref_free(w->modref);
    mario_free(w);
}

/* Resolve one guest import against the import table and append to the
 * synthetic module for its namespace. Sets *err on failure. */
static bool wasm_collect_import(vm_t* vm, WasmImport* im, EaImport* in,
                                var_t* ns_obj, const char** err) {
    var_t* v = get_obj(ns_obj, in->name);
    switch(in->kind) {
    case EAK_FUNC: {
        if(v == NULL || !v->is_func) { *err = "func import requires a JS function"; return false; }
        EaFuncType* declared = &im->mod.types[in->idx].func;
        EaFuncType* ft = &im->ftypes[im->n_funcs];
        ft->n_params = declared->n_params;
        ft->n_results = declared->n_results;
        ft->params = &im->type_bufs[im->n_buf];
        for(uint32_t k = 0; k < declared->n_params; k++) ft->params[k] = declared->params[k];
        im->n_buf += declared->n_params;
        ft->results = &im->type_bufs[im->n_buf];
        for(uint32_t k = 0; k < declared->n_results; k++) ft->results[k] = declared->results[k];
        im->n_buf += declared->n_results;
        WasmImportSlot* slot = &im->slots[im->n_funcs];
        slot->vm = vm;
        slot->fn = v;
        slot->type = ft;
        EaFuncInst* f = &im->funcs[im->n_funcs];
        f->type = ft;
        f->type_idx = 0;
        f->is_host = true;
        f->host_fn = wasm_import_call;
        f->host_user = slot;
        im->exports[im->n_exports].name = wasm_strdup(in->name);
        im->exports[im->n_exports].name_len = (uint32_t)strlen(in->name);
        im->exports[im->n_exports].kind = EAK_FUNC;
        im->exports[im->n_exports].idx = im->n_funcs;
        im->n_funcs++; im->n_exports++;
        return true;
    }
    case EAK_TABLE: {
        var_t* td = (v != NULL && wasm_is_kind(v, "table")) ? v : NULL;
        WasmTableData* tdata = (td != NULL) ? (WasmTableData*)td->value : NULL;
        if(tdata == NULL || tdata->ti == NULL) { *err = "table import requires a WebAssembly.Table"; return false; }
        if(tdata->ti->ref_type != in->table.ref_type) { *err = "table import element mismatch"; return false; }
        im->tables[im->n_tables] = *tdata->ti;
        im->exports[im->n_exports].name = wasm_strdup(in->name);
        im->exports[im->n_exports].name_len = (uint32_t)strlen(in->name);
        im->exports[im->n_exports].kind = EAK_TABLE;
        im->exports[im->n_exports].idx = im->n_tables;
        im->n_tables++; im->n_exports++;
        return true;
    }
    case EAK_MEMORY: {
        var_t* md = (v != NULL && wasm_is_kind(v, "memory")) ? v : NULL;
        WasmMemData* mdata = (md != NULL) ? (WasmMemData*)md->value : NULL;
        if(mdata == NULL || mdata->mi == NULL) { *err = "memory import requires a WebAssembly.Memory"; return false; }
        im->mems[im->n_mems] = mdata->mi;
        im->exports[im->n_exports].name = wasm_strdup(in->name);
        im->exports[im->n_exports].name_len = (uint32_t)strlen(in->name);
        im->exports[im->n_exports].kind = EAK_MEMORY;
        im->exports[im->n_exports].idx = im->n_mems;
        im->n_mems++; im->n_exports++;
        return true;
    }
    case EAK_GLOBAL: {
        var_t* gd = (v != NULL && wasm_is_kind(v, "global")) ? v : NULL;
        WasmGlobalData* gdata = (gd != NULL) ? (WasmGlobalData*)gd->value : NULL;
        if(gdata == NULL || gdata->slot == NULL) { *err = "global import requires a WebAssembly.Global"; return false; }
        im->globals[im->n_globals] = gdata->slot;
        im->globals_def[im->n_globals].type = gdata->type;
        im->globals_def[im->n_globals].mutable_ = gdata->mut;
        im->exports[im->n_exports].name = wasm_strdup(in->name);
        im->exports[im->n_exports].name_len = (uint32_t)strlen(in->name);
        im->exports[im->n_exports].kind = EAK_GLOBAL;
        im->exports[im->n_exports].idx = im->n_globals;
        im->n_globals++; im->n_exports++;
        return true;
    }
    default:
        *err = "tag imports are not supported by this bridge";
        return false;
    }
}

/* Build the instance.exports object. Exports borrow raw easm storage and are
 * valid only while the instance lives. */
static void wasm_build_exports(vm_t* vm, WasmInstance* w, var_t* exp) {
    EaInstance* inst = w->inst;
    EaModule* m = w->modref->mod;
    if(m == NULL) return;
    for(uint32_t i = 0; i < m->n_exports; i++) {
        EaExport* ex = &m->exports[i];
        var_t* v = NULL;
        switch(ex->kind) {
        case EAK_FUNC: {
            if(ex->idx >= inst->n_funcs) continue;
            EaFuncInst* fi = &inst->funcs[ex->idx];
            if(fi->is_host) continue;
            WasmFuncData* d = (WasmFuncData*)mario_malloc(sizeof(WasmFuncData));
            if(d == NULL) continue;
            d->store = w->store;
            d->inst = inst;
            d->func_idx = ex->idx;
            d->type = fi->type;
            v = var_new_native_func(vm, wasm_exported_call, d);
            if(v == NULL) { mario_free(d); continue; }
            v->on_destroy = wasm_func_destroy;
            wasm_mark(v, "func");
            break;
        }
        case EAK_MEMORY:
            if(ex->idx >= inst->n_memories || inst->memories[ex->idx] == NULL) continue;
            v = wasm_bare_obj(vm, WASM_CLS_MEMORY);
            if(v != NULL) {
                WasmMemData* d = (WasmMemData*)mario_malloc(sizeof(WasmMemData));
                if(d != NULL) {
                    d->mi = inst->memories[ex->idx]; d->owned = false;
                    v->value = d; v->free_func = wasm_mem_free;
                }
                wasm_mark(v, "memory");
            }
            break;
        case EAK_TABLE:
            if(ex->idx >= inst->n_tables) continue;
            v = wasm_bare_obj(vm, WASM_CLS_TABLE);
            if(v != NULL) {
                WasmTableData* d = (WasmTableData*)mario_malloc(sizeof(WasmTableData));
                if(d != NULL) {
                    d->ti = &inst->tables[ex->idx]; d->owned = false;
                    v->value = d; v->free_func = wasm_table_free;
                }
                wasm_mark(v, "table");
            }
            break;
        case EAK_GLOBAL:
            if(ex->idx >= inst->n_globals || inst->globals[ex->idx] == NULL) continue;
            if(ex->idx >= m->n_globals_def) continue;
            v = wasm_bare_obj(vm, WASM_CLS_GLOBAL);
            if(v != NULL) {
                WasmGlobalData* d = (WasmGlobalData*)mario_malloc(sizeof(WasmGlobalData));
                if(d != NULL) {
                    d->slot = inst->globals[ex->idx];
                    d->type = m->globals_def[ex->idx].type;
                    d->mut  = m->globals_def[ex->idx].mutable_;
                    d->owned = false;
                    v->value = d; v->free_func = wasm_global_free;
                }
                wasm_mark(v, "global");
            }
            break;
        default:
            continue;
        }
        if(v != NULL)
            var_add(exp, ex->name, v);
    }
}

/* Core instantiation: fill `this_v` (already Instance-prototyped) with the
 * WasmInstance and hidden exports. Returns this_v, or NULL after throwing. */
static var_t* wasm_build_instance(vm_t* vm, var_t* this_v, WasmModRef* r,
                                  var_t* import_obj) {
    WasmState* st = wasm_state(vm);
    if(st == NULL || r == NULL || r->mod == NULL) return NULL;
    EaModule* guest = r->mod;

    WasmInstance* w = (WasmInstance*)mario_malloc(sizeof(WasmInstance));
    if(w == NULL) return NULL;
    memset(w, 0, sizeof(*w));
    w->store = st->store;
    w->modref = r;
    r->refs++;

    var_t* import_root = var_new_array(vm);
    node_t* irn = var_add(this_v, WASM_IMPORTS_KEY, import_root);
    if(irn != NULL) { irn->invisable = 1; irn->be_unenumerable = 1; }

    const char* err = NULL;
    char* err_dyn = NULL;
    bool ok = true;
    WasmImport* last = NULL;

    for(uint32_t i = 0; i < guest->n_imports && ok; i++) {
        EaImport* in = &guest->imports[i];
        if(strcmp(in->module, "wasi_snapshot_preview1") == 0) {
            if(!wasm_ensure_wasi(vm, st)) {
                err = "wasi_snapshot_preview1 unavailable";
                ok = false;
            } else {
                w->wasi_registered = true;
            }
            continue;
        }
        WasmImport* im = w->imports;
        while(im != NULL && strcmp(im->name, in->module) != 0) im = im->next;
        if(im == NULL) {
            var_t* ns_obj = (import_obj != NULL && import_obj->type == V_OBJECT)
                                ? get_obj(import_obj, in->module) : NULL;
            if(ns_obj == NULL || ns_obj->type != V_OBJECT) {
                int nd = (int)strlen(in->module) + 48;
                err_dyn = (char*)mario_malloc((uint32_t)nd);
                if(err_dyn != NULL)
                    snprintf(err_dyn, (size_t)nd, "no import object provided for module '%s'",
                             in->module);
                err = err_dyn;
                ok = false;
                break;
            }
            im = (WasmImport*)mario_malloc(sizeof(WasmImport));
            if(im == NULL) { ok = false; break; }
            memset(im, 0, sizeof(*im));
            im->name = wasm_strdup(in->module);
            im->ns_obj = ns_obj;
            if(w->imports == NULL) w->imports = im; else last->next = im;
            last = im;

            uint32_t nf = 0, nt = 0, nm = 0, ng = 0;
            for(uint32_t j = 0; j < guest->n_imports; j++) {
                EaImport* jn = &guest->imports[j];
                if(strcmp(jn->module, in->module) != 0) continue;
                if(jn->kind == EAK_FUNC) nf++;
                else if(jn->kind == EAK_TABLE) nt++;
                else if(jn->kind == EAK_MEMORY) nm++;
                else if(jn->kind == EAK_GLOBAL) ng++;
            }
            im->exports = (EaExport*)mario_malloc((guest->n_imports ? guest->n_imports : 1) * sizeof(EaExport));
            im->ftypes = (EaFuncType*)mario_malloc((nf ? nf : 1) * sizeof(EaFuncType));
            im->type_bufs = (EaValType*)mario_malloc((nf ? nf : 1) * 16 * sizeof(EaValType));
            im->funcs = (EaFuncInst*)mario_malloc((nf ? nf : 1) * sizeof(EaFuncInst));
            im->tables = (EaTableInst*)mario_malloc((nt ? nt : 1) * sizeof(EaTableInst));
            im->mems = (EaMemInst**)mario_malloc((nm ? nm : 1) * sizeof(EaMemInst*));
            im->globals = (WVal**)mario_malloc((ng ? ng : 1) * sizeof(WVal*));
            im->globals_def = (EaGlobal*)mario_malloc((ng ? ng : 1) * sizeof(EaGlobal));
            im->slots = (WasmImportSlot*)mario_malloc((nf ? nf : 1) * sizeof(WasmImportSlot));
            /* synthetic module: no type space -> func imports match structurally */
            im->mod.n_types = 0;
            im->mod.types = NULL;
            im->mod.funcs = im->funcs;
            im->mod.tables = im->tables;
            im->mod.memories = im->mems;
            im->mod.globals_def = im->globals_def;
            im->mod.exports = im->exports;
            /* mod.n_* updated as exports accumulate */
            if(!wasm_collect_import(vm, im, in, ns_obj, &err)) { ok = false; break; }
            var_array_add(import_root, ns_obj);
        } else {
            var_t* ns_obj = (import_obj != NULL && import_obj->type == V_OBJECT)
                                ? get_obj(import_obj, in->module) : NULL;
            if(!wasm_collect_import(vm, im, in, (ns_obj != NULL) ? ns_obj : im->ns_obj, &err)) {
                ok = false;
                break;
            }
        }
    }
    /* attach accumulated counts to each synthetic module */
    for(WasmImport* im = w->imports; im != NULL; im = im->next) {
        im->mod.n_funcs = im->n_funcs;
        im->mod.n_tables = im->n_tables;
        im->mod.n_memories = im->n_mems;
        im->mod.n_globals_def = im->n_globals;
        im->mod.n_exports = im->n_exports;
    }

    EaInstance* inst = NULL;
    char* err2 = NULL;
    EaTrap trap = TRAP_NONE;
    int rc = -1;
    if(ok) {
        for(WasmImport* im = w->imports; im != NULL; im = im->next) {
            if(im->n_exports == 0) continue;
            EaInstance* src = (EaInstance*)mario_malloc(sizeof(EaInstance));
            if(src == NULL) { ok = false; break; }
            memset(src, 0, sizeof(*src));
            src->module = &im->mod;
            src->n_funcs = im->n_funcs;     src->funcs = im->funcs;
            src->n_tables = im->n_tables;   src->tables = im->tables;
            src->n_memories = im->n_mems;   src->memories = im->mems;
            src->n_globals = im->n_globals; src->globals = im->globals;
            src->n_tags = 0;                src->tags = NULL;
            src->start_func = UINT32_MAX;
            im->registered = src;
            ea_register_instance(st->store, im->name, src);
        }
        if(rc != -1) { /* unreachable guard */ }
        if(ok)
            rc = ea_store_instantiate(st->store, guest, &inst, &err2, &trap);
        for(WasmImport* im = w->imports; im != NULL; im = im->next)
            if(im->n_exports > 0) ea_store_unregister(st->store, im->name);
    }

    if(!ok || rc != 0) {
        if(err == NULL && err2 != NULL) { err_dyn = err2; err = err_dyn; }
        vm_throw_type_native(vm, WASM_CLS_LE, "LinkError: %s",
                             (err != NULL) ? err : "unknown import");
        if(err_dyn != NULL) { mario_free(err_dyn); }
        wasm_instance_free(w);
        return NULL;
    }
    if(err2 != NULL) mario_free(err2);

    w->inst = inst;
    if(w->wasi_registered && st->wasi != NULL)
        ea_wasi_bind(st->wasi, inst);

    this_v->value = w;
    this_v->free_func = wasm_instance_free;

    var_t* exp = var_new_obj_no_proto(vm, NULL, NULL);
    wasm_build_exports(vm, w, exp);
    node_t* en = var_add(this_v, WASM_EXPORTS_KEY, exp);
    if(en != NULL) { en->invisable = 1; en->be_unenumerable = 1; }
    return this_v;
}

static var_t* native_Instance_Constructor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* this_v = js_this(env);
    var_t* modv = js_arg(env, 0);
    var_t* import_obj = js_arg(env, 1);
    if(modv == NULL || modv->type != V_OBJECT || !wasm_is_kind(modv, "module") ||
       modv->value == NULL) {
        vm_throw_type_native(vm, "TypeError", "WebAssembly.Instance requires a Module");
        return this_v;
    }
    if(wasm_build_instance(vm, this_v, (WasmModRef*)modv->value, import_obj) == NULL)
        return this_v;
    wasm_mark(this_v, "instance");
    return this_v;
}

static var_t* native_Instance_get_exports(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = js_this(env);
    if(self == NULL) return var_new_obj_no_proto(vm, NULL, NULL);
    var_t* e = get_obj(self, WASM_EXPORTS_KEY);
    return (e != NULL) ? e : var_new_obj_no_proto(vm, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* Namespace statics                                                 */
/* ------------------------------------------------------------------ */

static var_t* native_validate(vm_t* vm, var_t* env, void* data) {
    (void)data;
    const uint8_t* bytes = NULL;
    uint32_t len = 0;
    if(!wasm_bytes_of(js_arg(env, 0), &bytes, &len)) {
        vm_throw_type_native(vm, "TypeError",
                             "WebAssembly.validate requires a buffer source");
        return var_new_bool(vm, false);
    }
    EaModule m;
    memset(&m, 0, sizeof(m));
    char* err = NULL;
    int rc = ea_decode_module(&m, bytes, len, &err);
    if(rc == 0) rc = ea_validate_module(&m, &err);
    if(err != NULL) free(err);
    bool ok = (rc == 0);
    if(m.owned_bytes != NULL) ea_module_free(&m);
    return var_new_bool(vm, ok);
}

static var_t* wasm_promise(vm_t* vm, const char* which, var_t* value) {
    var_t* cls = var_find_own_member_var(vm->root, "Promise");
    if(cls == NULL) return value;
    var_t* fn = get_obj(cls, which);
    if(fn == NULL || !fn->is_func) fn = get_obj(var_get_prototype(cls), which);
    if(fn == NULL || !fn->is_func) return value;
    var_t* args = var_new_array(vm);
    var_array_add(args, value);
    var_t* p = call_m_func(vm, cls, fn, args);
    var_unref(args);
    return (p != NULL) ? p : value;
}

static var_t* native_compile(vm_t* vm, var_t* env, void* data) {
    (void)data;
    const uint8_t* bytes = NULL;
    uint32_t len = 0;
    if(!wasm_bytes_of(js_arg(env, 0), &bytes, &len)) {
        vm_throw_type_native(vm, "TypeError",
                             "WebAssembly.compile requires a buffer source");
        return var_new_null(vm);
    }
    WasmState* st = wasm_state(vm);
    EaModule* m = NULL;
    if(!wasm_compile_module(vm, st, bytes, len, &m))
        return var_new_null(vm);
    var_t* mv = wasm_module_obj(vm, m);
    if(mv == NULL) return var_new_null(vm);
    return wasm_promise(vm, "resolve", mv);   /* the promise owns mv */
}

/* helper used by instantiate: returns Instance object or NULL after throwing. */
static var_t* wasm_make_instance(vm_t* vm, WasmModRef* r, var_t* import_obj) {
    var_t* iv = wasm_bare_obj(vm, WASM_CLS_INSTANCE);
    if(iv == NULL) return NULL;
    if(wasm_build_instance(vm, iv, r, import_obj) == NULL) {
        var_unref(iv);
        return NULL;
    }
    wasm_mark(iv, "instance");
    return iv;
}

static var_t* native_instantiate(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* arg = js_arg(env, 0);
    var_t* import_obj = js_arg(env, 1);
    var_t* mv = NULL;
    WasmModRef* ref = NULL;
    bool owns_mv = false;

    if(arg != NULL && arg->type == V_OBJECT && wasm_is_kind(arg, "module") &&
       arg->value != NULL) {
        mv = arg;
        ref = (WasmModRef*)arg->value;
    } else {
        const uint8_t* bytes = NULL;
        uint32_t len = 0;
        if(!wasm_bytes_of(arg, &bytes, &len)) {
            vm_throw_type_native(vm, "TypeError",
                                 "WebAssembly.instantiate requires bytes or a Module");
            return var_new_null(vm);
        }
        WasmState* st = wasm_state(vm);
        EaModule* m = NULL;
        if(!wasm_compile_module(vm, st, bytes, len, &m))
            return var_new_null(vm);
        mv = wasm_module_obj(vm, m);
        if(mv == NULL) return var_new_null(vm);
        ref = (WasmModRef*)mv->value;
        owns_mv = true;
    }
    if(ref == NULL) return var_new_null(vm);

    var_t* iv = wasm_make_instance(vm, ref, import_obj);
    if(iv == NULL) {
        if(owns_mv) var_unref(mv);
        return var_new_null(vm);
    }

    var_t* pair = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(pair, "module", mv);
    var_add(pair, "instance", iv);
    var_t* p = wasm_promise(vm, "resolve", pair);
    var_unref(pair);   /* pair + its children released; promise adopts them */
    return p;
}

static var_t* native_streaming_unsupported(vm_t* vm, var_t* env, void* data) {
    (void)data; (void)env;
    var_t* reason = var_new_str(vm,
        "CompileError: streaming compilation is unsupported; use WebAssembly.instantiate(bytes, imports)");
    var_t* p = wasm_promise(vm, "reject", reason);
    if(p == NULL) p = reason;
    return p;
}

/* ------------------------------------------------------------------ */
/* Error classes + registration                                      */
/* ------------------------------------------------------------------ */

static var_t* wasm_error_ctor(vm_t* vm, var_t* env, void* data) {
    const char* name = (const char*)data;
    var_t* self = js_this(env);
    if(self == NULL) return NULL;
    var_t* msg = js_arg(env, 0);
    var_add(self, "name", var_new_str(vm, name));
    if(msg != NULL && msg->type != V_UNDEF) {
        mstr_t* n = mstr_new("");
        const char* cs = js_cstr(msg, n);
        var_add(self, "message", var_new_str(vm, cs));
        mstr_free(n);
    } else {
        var_add(self, "message", var_new_str(vm, ""));
    }
    return self;
}

static void wasm_add_error_class(vm_t* vm, var_t* wasm_ns, var_t* error_proto,
                                 const char* name) {
    var_t* cls = vm_new_class(vm, name);
    if(cls == NULL) return;
    vm_reg_native(vm, cls, "constructor(message)", wasm_error_ctor, (void*)name);
    var_t* proto = var_get_prototype(cls);
    if(proto != NULL && error_proto != NULL)
        var_set_prototype(proto, error_proto);
    if(wasm_ns != NULL)
        var_add(wasm_ns, name, cls);
}

bool js_register_wasm_natives(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return false;
    if(var_find_own_member_var(vm->root, "WebAssembly") != NULL) return true;

    WasmState* st = (WasmState*)mario_malloc(sizeof(WasmState));
    if(st == NULL) return false;
    memset(st, 0, sizeof(*st));
    st->vm = vm;
    st->store = ea_store_new();
    if(st->store == NULL) {
        mario_free(st);
        return false;
    }

    vm->gc.gc_defer++;
    var_t* bridge = var_new_obj_no_proto(vm, st, wasm_state_free);
    if(bridge == NULL) {
        ea_store_free(st->store);
        mario_free(st);
        vm->gc.gc_defer--;
        return false;
    }
    var_add(vm->root, WASM_BRIDGE_KEY, bridge);

    var_t* ns = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(vm->root, "WebAssembly", ns);

    var_t* error_proto = wasm_class_proto(vm, "Error");
    wasm_add_error_class(vm, ns, error_proto, WASM_CLS_CE);
    wasm_add_error_class(vm, ns, error_proto, WASM_CLS_LE);
    wasm_add_error_class(vm, ns, error_proto, WASM_CLS_RE);

    var_t* mod_cls  = vm_new_class(vm, WASM_CLS_MODULE);
    var_t* inst_cls = vm_new_class(vm, WASM_CLS_INSTANCE);
    var_t* mem_cls  = vm_new_class(vm, WASM_CLS_MEMORY);
    var_t* tab_cls  = vm_new_class(vm, WASM_CLS_TABLE);
    var_t* glo_cls  = vm_new_class(vm, WASM_CLS_GLOBAL);

    vm_reg_native(vm, mod_cls,  "constructor(bytes)", native_Module_constructor, NULL);
    js_acc_cls(vm, mod_cls, "exports", native_Module_get_exports, NULL, NULL);
    js_acc_cls(vm, mod_cls, "imports", native_Module_get_imports, NULL, NULL);
    vm_reg_static(vm, mod_cls, "customSections(module, sectionName)",
                  native_Module_customSections, NULL);

    vm_reg_native(vm, inst_cls, "constructor(module, imports)", native_Instance_Constructor, NULL);
    js_acc_cls(vm, inst_cls, "exports", native_Instance_get_exports, NULL, NULL);

    vm_reg_native(vm, mem_cls, "constructor(descriptor)", native_Memory_constructor, NULL);
    js_acc_cls(vm, mem_cls, "buffer", native_Memory_get_buffer, NULL, NULL);
    vm_reg_native(vm, mem_cls, "grow(delta)", native_Memory_grow, NULL);

    vm_reg_native(vm, tab_cls, "constructor(descriptor)", native_Table_Constructor, NULL);
    js_acc_cls(vm, tab_cls, "length", native_Table_get_length, NULL, NULL);
    vm_reg_native(vm, tab_cls, "get(index)", native_Table_get, NULL);
    vm_reg_native(vm, tab_cls, "set(index, value)", native_Table_set, NULL);
    vm_reg_native(vm, tab_cls, "grow(delta, value)", native_Table_grow, NULL);

    vm_reg_native(vm, glo_cls, "constructor(descriptor, value)", native_Global_Constructor, NULL);
    js_acc_cls(vm, glo_cls, "value", native_Global_get_value, native_Global_set_value, NULL);
    vm_reg_native(vm, glo_cls, "valueOf()", native_Global_valueOf, NULL);

    vm_reg_native_on(vm, ns, "validate(bytes)", native_validate, NULL);
    vm_reg_native_on(vm, ns, "compile(bytes)", native_compile, NULL);
    vm_reg_native_on(vm, ns, "instantiate(bytes, imports)", native_instantiate, NULL);
    vm_reg_native_on(vm, ns, "instantiateStreaming(source, imports)", native_streaming_unsupported, NULL);
    vm_reg_native_on(vm, ns, "compileStreaming(source)", native_streaming_unsupported, NULL);

    var_add(ns, "Module", mod_cls);
    var_add(ns, "Instance", inst_cls);
    var_add(ns, "Memory", mem_cls);
    var_add(ns, "Table", tab_cls);
    var_add(ns, "Global", glo_cls);

    var_t* win = var_find_own_member_var(vm->root, "window");
    if(win != NULL)
        var_add(win, "WebAssembly", ns);

    vm->gc.gc_defer--;
    return true;
}

#ifdef __cplusplus
}
#endif