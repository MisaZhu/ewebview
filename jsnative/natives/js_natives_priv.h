/*
 * js_natives_priv.h - helpers shared by the browser native bridges
 *                     (js_dom.c / js_event.c / js_web.c).
 *
 * Internal to browser/libs/mario/natives: this header is NOT installed into
 * the SDK, it only factors out the mario plumbing every bridge repeats
 * (accessor properties, argument coercion, small string utilities).
 *
 * Everything is `static inline` so each translation unit gets a private copy
 * and no extra object file has to be added to libmario.a.
 */

#ifndef MARIO_JS_NATIVES_PRIV_H
#define MARIO_JS_NATIVES_PRIV_H

#include "mario.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Accessor properties                                                */
/*                                                                    */
/* mario's public vm_reg_* API only creates regular methods, but the   */
/* runtime understands ES6 accessors: a read of a member whose         */
/* func.regular == FUNC_GETTER calls it with `this` = the object       */
/* (mario.c get_member -> func_call), and an assignment resolves the   */
/* setter through FUNC_SETTER_KEY (see merge_accessor). These two      */
/* helpers reproduce that layout by hand.                              */
/* ------------------------------------------------------------------ */

/* Install `<prop>` directly on `target` (a class PROTOTYPE, or a plain
 * object such as `window`). `setter` may be NULL for a read-only property. */
static inline void js_acc_on(vm_t* vm, var_t* target, const char* prop,
                             native_func_t getter, native_func_t setter, void* data) {
    if(target == NULL) return;
    char decl[96];
    snprintf(decl, sizeof(decl), "%s()", prop);
    node_t* gn = vm_reg_native_on(vm, target, decl, getter, data);
    if(gn == NULL || gn->var == NULL) return;
    func_t* gf = var_get_func(gn->var);
    if(gf != NULL) gf->regular = FUNC_GETTER;
    if(setter == NULL) return;

    node_t* sn = vm_reg_native_on(vm, gn->var, FUNC_SETTER_KEY "(v)", setter, data);
    if(sn == NULL || sn->var == NULL) return;
    func_t* sf = var_get_func(sn->var);
    if(sf != NULL) sf->regular = FUNC_SETTER;
    sn->invisable = 1;
    sn->be_unenumerable = 1;
}

/* Same, but `cls` is a class var as returned by vm_new_class(): the member is
 * installed on its prototype so every instance inherits it. */
static inline void js_acc_cls(vm_t* vm, var_t* cls, const char* prop,
                              native_func_t getter, native_func_t setter, void* data) {
    if(cls == NULL) return;
    js_acc_on(vm, var_get_prototype(cls), prop, getter, setter, data);
}

/* ------------------------------------------------------------------ */
/* Argument / value coercion                                          */
/* ------------------------------------------------------------------ */

static inline var_t* js_this(var_t* env) {
    return (env == NULL) ? NULL : get_obj(env, THIS);
}

static inline int js_arg_count(var_t* env) {
    return (int)get_func_args_num(env);
}

/* Argument `idx` of the current call, or NULL when absent. */
static inline var_t* js_arg(var_t* env, int idx) {
    if(idx < 0) return NULL;
    return get_func_arg(env, (uint32_t)idx);
}

/* JS ToString() of `v` as a C string. Strings are returned as-is; anything
 * else is converted into `scratch` (which the caller owns and must have
 * created with mstr_new). Never NULL. */
static inline const char* js_cstr(var_t* v, mstr_t* scratch) {
    if(v == NULL) return "";
    if(v->type == V_STRING) {
        const char* s = var_get_str(v);
        return (s != NULL) ? s : "";
    }
    if(scratch == NULL) return "";
    var_to_str(v, scratch);
    return scratch->cstr;
}

/* Convenience wrapper: ToString() of argument `idx`. */
static inline const char* js_arg_cstr(var_t* env, int idx, mstr_t* scratch) {
    return js_cstr(js_arg(env, idx), scratch);
}

/* JS ToNumber() of `v`. Unlike var_get_float64() this also converts strings
 * ("12px" -> 12, "1.5em" -> 1.5), which is what the CSS/DOM entry points
 * receive from scripts. NaN/undefined/null collapse to 0. */
static inline double js_num(var_t* v) {
    if(v == NULL || v->value == NULL) return 0.0;
    switch(v->type) {
        case V_STRING: return (double)mstr_to_float(var_get_str(v));
        case V_BOOL:   return var_get_bool(v) ? 1.0 : 0.0;
        case V_NULL:   return 0.0;
        default:       return var_get_float64(v);
    }
}

static inline int js_arg_int(var_t* env, int idx) {
    return (int)js_num(js_arg(env, idx));
}

static inline double js_arg_num(var_t* env, int idx) {
    return js_num(js_arg(env, idx));
}

/* Argument `idx` as a JS function var, or NULL. */
static inline var_t* js_arg_func(var_t* env, int idx) {
    var_t* v = js_arg(env, idx);
    if(v == NULL || !v->is_func) return NULL;
    return v;
}

/* JS ToBoolean() of `v`. */
static inline bool js_truthy(var_t* v) {
    if(v == NULL) return false;
    switch(v->type) {
        case V_UNDEF:
        case V_NULL:   return false;
        case V_BOOL:   return var_get_bool(v);
        case V_STRING: {
            const char* s = var_get_str(v);
            return s != NULL && s[0] != 0;
        }
        case V_INT:    return var_get_int(v) != 0;
        case V_INT64:  return var_get_int64(v) != 0;
        case V_FLOAT:  return var_get_float(v) != 0.0f;
        case V_FLOAT64: {
            double d = var_get_float64(v);
            return d != 0.0 && d == d;   /* NaN is falsy */
        }
        default:       return true;      /* objects are always truthy */
    }
}

/* ------------------------------------------------------------------ */
/* Small string / memory utilities                                    */
/* ------------------------------------------------------------------ */

/* mario_malloc'd copy of `s` (NULL-safe: returns NULL). Callers that hand the
 * result to a native which adopts it must use mario_free(). */
static inline char* js_strdup(const char* s) {
    if(s == NULL) return NULL;
    uint32_t n = (uint32_t)strlen(s);
    char* p = (char*)mario_malloc(n + 1);
    if(p == NULL) return NULL;
    if(n > 0) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* mario_malloc'd copy of at most `n` bytes of `s`, always NUL-terminated. */
static inline char* js_strndup(const char* s, uint32_t n) {
    if(s == NULL) n = 0;
    else {
        uint32_t l = (uint32_t)strlen(s);
        if(l < n) n = l;
    }
    char* p = (char*)mario_malloc(n + 1);
    if(p == NULL) return NULL;
    if(n > 0) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* Case-insensitive ASCII compare (avoids pulling strings.h/strcasecmp, which
 * is not uniformly available across the cross toolchains). */
static inline int js_ascii_casecmp(const char* a, const char* b) {
    if(a == NULL) a = "";
    if(b == NULL) b = "";
    while(*a != 0 && *b != 0) {
        char ca = *a, cb = *b;
        if(ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if(cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if(ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline char js_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* The mirror of js_ascii_lower. Needed because HTML parsers commonly fold tag
 * names to lower case internally while the DOM specifies Element.tagName and
 * Node.nodeName as UPPER case for HTML elements - see native_el_get_tagName. */
static inline char js_ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Grow a mstr_t by appending `n` bytes of `s`. */
static inline void js_mstr_append(mstr_t* m, const char* s, uint32_t n) {
    for(uint32_t i = 0; i < n; ++i) mstr_add(m, s[i]);
}

#endif /* MARIO_JS_NATIVES_PRIV_H */
