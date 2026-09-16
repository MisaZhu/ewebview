/*
 * js_dom.c - Browser DOM natives for the mario JavaScript VM.
 *
 * See js_dom.h for the contract. This file is pure C and depends only on
 * mario.h; all DOM knowledge comes in through js_dom_callbacks_t.
 *
 * Object model:
 *   - A hidden "DomBridge" var hangs off vm->root under DOM_BRIDGE_KEY and
 *     carries the ctx pointer + callbacks + the document.write buffer in
 *     its ->value (a heap js_dom_state). Every native receives it via the
 *     `data` argument registered with vm_reg_*, so no globals are needed
 *     and multiple VMs can coexist.
 *   - `document` is an instance of class "Document"; `window.location` is a
 *     plain object with an `href` getter.
 *   - Element wrappers are instances of class "Element" whose ->value holds
 *     the opaque js_element_t handle. The handle is NOT owned by the VM:
 *     el_free is a no-op, and the embedder guarantees validity only for the
 *     duration of a script run.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "js_dom.h"
#include "js_natives_priv.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define CLS_DOCUMENT "Document"
#define CLS_ELEMENT  "Element"
#define CLS_LOCATION "Location"
#define CLS_MATH     "Math"
#define CLS_DATE     "Date"
#define CLS_STYLE    "CSSStyleDeclaration"
#define CLS_TOKENS   "DOMTokenList"

/* Key under which the bridge state is stashed on vm->root. Prefixed with @@
 * to match mario's convention for hidden members (see FUNC_SETTER_KEY). */
#define DOM_BRIDGE_KEY   "@@dom_bridge"
/* Hidden array on the bridge var mirroring every live timer callback so the
 * GC (which marks from vm->root and ignores refcounts) keeps them reachable
 * across script runs - the same trick native_Promise.c uses with @@keep. */
#define DOM_TIMERS_KEY   "@@timers"
/* Hidden array anchoring every cached Element wrapper (see wrap_element). */
#define DOM_ELCACHE_KEY  "@@elcache"

/* Upper bound on concurrently scheduled timers. A fixed table keeps the hot
 * path allocation-free; 32 is far beyond what any real page schedules. */
#define JS_TIMER_MAX 32

/* Element wrapper identity cache. Handing back the SAME var for the same
 * embedder handle is what makes `e.target === el` and `a === b` behave like a
 * real browser; without it every getElementById() builds a fresh object and
 * identity comparisons always fail. The table GROWS and is never evicted:
 * the anchor array is the only root keeping a wrapper alive while a script
 * holds it across a GC, so dropping an entry (a previous 128-slot ring did)
 * lets the VM recycle memory a live JS variable still points at - the wrapper
 * then reads back as a prototype-less plain object and every member access
 * on it yields undefined. Cleared only when the document goes away. */

typedef struct {
    js_element_t          handle;       /* NULL when the slot is empty */
    var_t*                obj;          /* anchored via DOM_ELCACHE_KEY */
} js_el_cache_t;

typedef struct {
    int                   id;           /* value handed to JS (never 0) */
    var_t*                cb;           /* anchored via DOM_TIMERS_KEY */
    uint32_t              period_ms;    /* interval between fires */
    int64_t               remaining_ms; /* counts down each poll; due at <= 0 */
    bool                  repeat;       /* setInterval vs setTimeout */
    bool                  active;       /* slot in use */
} js_timer_t;

typedef struct {
    void*                 ctx;
    js_dom_callbacks_t    cb;
    mstr_t*               write_buf;   /* document.write accumulator */
    js_timer_t            timers[JS_TIMER_MAX];
    int                   timer_next_id;  /* monotonic id source */
    uint64_t              timer_last_now; /* embedder clock at the previous poll */
    bool                  timer_synced;   /* false until the first poll seeds it */
    js_el_cache_t*        el_cache;       /* growable, never evicted (see above) */
    int                   el_cache_len;
    int                   el_cache_cap;
} js_dom_state;

/* ------------------------------------------------------------------ */
/* Bridge state plumbing                                              */
/* ------------------------------------------------------------------ */

static void dom_state_free(void* p) {
    js_dom_state* st = (js_dom_state*)p;
    if(st == NULL) return;
    if(st->write_buf != NULL) mstr_free(st->write_buf);
    if(st->el_cache != NULL) mario_free(st->el_cache);
    mario_free(st);
}

/* Pull the bridge state out of a native's `data` argument. Every native in
 * this file is registered with data == the bridge var, so this is O(1). */
static js_dom_state* state_from_data(void* data) {
    if(data == NULL) return NULL;
    var_t* bridge = (var_t*)data;
    return (js_dom_state*)bridge->value;
}

/* Some mario call paths hand the native the *env* rather than the
 * registered data when the function is invoked as a method (obj.foo()).
 * In that case `this` is the Document/Element instance; walk up to the
 * bridge via the vm root. */
static js_dom_state* state_from_vm(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return NULL;
    var_t* bridge = var_find_own_member_var(vm->root, DOM_BRIDGE_KEY);
    if(bridge == NULL) return NULL;
    return (js_dom_state*)bridge->value;
}

static js_dom_state* state_any(vm_t* vm, void* data) {
    js_dom_state* st = state_from_data(data);
    if(st != NULL) return st;
    return state_from_vm(vm);
}

/* Copy a callback-returned mario_malloc'd string into a fresh JS string
 * var, freeing the C buffer. NULL becomes JS null. */
static var_t* adopt_cstr(vm_t* vm, char* s) {
    if(s == NULL) return var_new_null(vm);
    var_t* v = var_new_str(vm, s);
    mario_free(s);
    return v;
}

/* Register a getter/setter accessor property on a class prototype.
 * Thin wrapper over the shared helper in js_natives_priv.h, kept so the
 * existing call sites below read the same as before. */
static void reg_accessor(vm_t* vm, var_t* cls, const char* prop,
                         native_func_t getter, native_func_t setter, void* data) {
    js_acc_cls(vm, cls, prop, getter, setter, data);
}

/* ------------------------------------------------------------------ */
/* alert()                                                            */
/* ------------------------------------------------------------------ */

static var_t* native_alert(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    mstr_t* msg = mstr_new("");
    mstr_t* one = mstr_new("");
    uint32_t sz = var_array_size(args);
    for(uint32_t i = 0; i < sz; ++i) {
        node_t* n = var_array_get(args, i);
        if(n != NULL) {
            var_to_str(n->var, one);
            if(i > 0) mstr_add(msg, ' ');
            mstr_append(msg, one->cstr);
        }
    }
    mstr_free(one);
    if(st != NULL && st->cb.alert != NULL) {
        st->cb.alert(st->ctx, msg->cstr);
    } else {
        /* No embedder hook: fall back to console so the message is not lost. */
        mstr_add(msg, '\n');
        _platform_out(msg->cstr);
    }
    mstr_free(msg);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* eval(src)                                                          */
/*                                                                     */
/* The engine has no lexical-scope eval; run the source in the global  */
/* scope instead (what page feature-detects actually need). The        */
/* expression form is tried first by assigning into a hidden global    */
/* (`eval("1+1")` yields 2); if that does not compile, the source runs */
/* as plain statements and yields undefined. Non-string arguments are  */
/* returned unchanged, per spec.                                       */
/* ------------------------------------------------------------------ */

#define EVAL_RET_KEY "@@evalret"

static var_t* native_eval(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* args = get_func_args(env);
    node_t* n = (args != NULL) ? var_array_get(args, 0) : NULL;
    if(n == NULL || n->var == NULL)
        return NULL;
    if(n->var->type != V_STRING)
        return n->var; /* borrowed, like native_Object_getPrototypeOf */
    const char* src = var_get_str(n->var);
    if(src == NULL || src[0] == 0)
        return NULL;

    mstr_t* code = mstr_new(EVAL_RET_KEY " = (");
    mstr_append(code, src);
    mstr_append(code, "\n);"); /* the newline guards a trailing // comment */
    /* The expression-form attempt is speculative: a statement-shaped body is
     * expected to fail here and be retried below, so silence its diagnostics. */
    extern void js_compile_set_quiet(bool quiet);
    js_compile_set_quiet(true);
    bool ok = vm_load_run_native(vm, code->cstr);
    js_compile_set_quiet(false);
    mstr_free(code);
    if(!ok) {
        vm_load_run_native(vm, src);
        return NULL;
    }
    return var_find_own_member_var(vm->root, EVAL_RET_KEY); /* borrowed */
}

/* ------------------------------------------------------------------ */
/* document.write / writeln                                           */
/* ------------------------------------------------------------------ */

static void write_append(js_dom_state* st, const char* s, bool newline) {
    if(st == NULL || s == NULL) return;
    if(st->write_buf == NULL) st->write_buf = mstr_new("");
    mstr_append(st->write_buf, s);
    if(newline) mstr_add(st->write_buf, '\n');
    if(st->cb.document_write != NULL) {
        st->cb.document_write(st->ctx, s);
    }
}

static var_t* native_document_write(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    mstr_t* buf = mstr_new("");
    mstr_t* one = mstr_new("");
    uint32_t sz = var_array_size(args);
    for(uint32_t i = 0; i < sz; ++i) {
        node_t* n = var_array_get(args, i);
        if(n != NULL) {
            var_to_str(n->var, one);
            mstr_append(buf, one->cstr);
        }
    }
    mstr_free(one);
    write_append(st, buf->cstr, false);
    mstr_free(buf);
    return NULL;
}

static var_t* native_document_writeln(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    mstr_t* buf = mstr_new("");
    mstr_t* one = mstr_new("");
    uint32_t sz = var_array_size(args);
    for(uint32_t i = 0; i < sz; ++i) {
        node_t* n = var_array_get(args, i);
        if(n != NULL) {
            var_to_str(n->var, one);
            mstr_append(buf, one->cstr);
        }
    }
    mstr_free(one);
    write_append(st, buf->cstr, true);
    mstr_free(buf);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* document.title (getter/setter)                                     */
/* ------------------------------------------------------------------ */

static var_t* native_document_get_title(vm_t* vm, var_t* env, void* data) {
    (void)env;
    js_dom_state* st = state_any(vm, data);
    if(st == NULL || st->cb.get_title == NULL) return var_new_str(vm, "");
    return adopt_cstr(vm, st->cb.get_title(st->ctx));
}

static var_t* native_document_set_title(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    node_t* n = var_array_get(args, 0);
    const char* s = (n != NULL) ? var_get_str(n->var) : "";
    if(st != NULL && st->cb.set_title != NULL) {
        st->cb.set_title(st->ctx, s ? s : "");
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* document.getElementById(id) -> Element | null                      */
/* ------------------------------------------------------------------ */

/* Forward: build an Element wrapper var around an opaque handle. */
static var_t* wrap_element(vm_t* vm, js_element_t el);

static var_t* native_document_getElementById(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    node_t* n = var_array_get(args, 0);
    if(n == NULL || st == NULL || st->cb.get_element_by_id == NULL) {
        return var_new_null(vm);
    }
    const char* id = var_get_str(n->var);
    if(id == NULL || id[0] == 0) return var_new_null(vm);
    js_element_t el = st->cb.get_element_by_id(st->ctx, id);
    if(el == NULL) return var_new_null(vm);
    return wrap_element(vm, el);
}

/* document.body / head / documentElement live further down, next to the rest
 * of the document accessors (see doc_node()). */

/* ------------------------------------------------------------------ */
/* Element class natives                                              */
/* ------------------------------------------------------------------ */

/* The Element instance's ->value is the opaque handle. el_free is a
 * no-op because the embedder owns the underlying DOM node. */
static void el_free(void* p) { (void)p; }

static js_element_t handle_from_this(var_t* this_v) {
    if(this_v == NULL) return NULL;
    /* Wrapper-identity guard. Only wrap_element() installs el_free as the value's
     * free_func, so this rejects an impostor that merely looks like an Element
     * (Object.create(Element.prototype), a user subclass, or a var whose memory
     * the GC recycled into an ordinary object). Such a var's ->value is a live
     * mario pointer, not an embedder handle; the embedder would cast it to a
     * litehtml::element, splice it into the tree and abort in the style walk.
     * See the object-model note at the top of this file. */
    if(this_v->free_func != el_free) return NULL;
    js_element_t el = (js_element_t)this_v->value;
    if(el == NULL) return NULL;
    /* Liveness gate. The wrapper can outlive the element it wrapped (freed on a
     * document swap or an innerHTML/subtree rewrite) while the script keeps
     * using it, and the VM recycles that memory for its own objects. Ask the
     * embedder whether the handle is still a live element before returning it,
     * so a dangling pointer degrades to null instead of being dereferenced. */
    js_dom_state* st = state_from_vm(this_v->vm);
    if(st != NULL && st->cb.el_is_live != NULL && !st->cb.el_is_live(st->ctx, el))
        return NULL;
    return el;
}

static var_t* dom_bridge_var(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return NULL;
    return var_find_own_member_var(vm->root, DOM_BRIDGE_KEY);
}

/* Keep every cached wrapper reachable from vm->root. The GC marks from the
 * root and frees anything unmarked, so an unanchored cache would dangle the
 * moment a script dropped its last reference to an element. var_add replaces
 * the previous anchor array (node_replace refs the new, unrefs the old); the
 * gc_defer window covers the instant the entries are momentarily unreachable. */
static void js_reanchor_el_cache(vm_t* vm, js_dom_state* st) {
    var_t* bridge = dom_bridge_var(vm);
    if(bridge == NULL) return;
    vm->gc.gc_defer++;
    var_t* fresh = var_new_array(vm);
    for(int i = 0; i < st->el_cache_len; ++i)
        if(st->el_cache[i].handle != NULL && st->el_cache[i].obj != NULL)
            var_array_add(fresh, st->el_cache[i].obj);
    node_t* n = var_add(bridge, DOM_ELCACHE_KEY, fresh);
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
    vm->gc.gc_defer--;
}

static var_t* wrap_element(vm_t* vm, js_element_t el) {
    js_dom_state* st = state_from_vm(vm);
    if(st != NULL) {
        for(int i = 0; i < st->el_cache_len; ++i)
            if(st->el_cache[i].handle == el && st->el_cache[i].obj != NULL)
                return st->el_cache[i].obj;   /* rooted by the anchor array */
    }
    /* new_obj() constructs an instance of the registered "Element" class
     * (prototype + constructor already wired by js_register_dom_natives).
     * We then stash the handle in ->value so the method natives can recover
     * it via `this`. */
    var_t* obj = new_obj(vm, CLS_ELEMENT, 0);
    if(obj == NULL) return var_new_null(vm);
    obj->value = el;
    obj->free_func = el_free;
    if(st != NULL) {
        if(st->el_cache_len == st->el_cache_cap) {
            int ncap = (st->el_cache_cap > 0) ? st->el_cache_cap * 2 : 64;
            js_el_cache_t* nn = (js_el_cache_t*)mario_malloc(sizeof(js_el_cache_t) * (size_t)ncap);
            if(nn != NULL) {
                for(int i = 0; i < st->el_cache_len; ++i) nn[i] = st->el_cache[i];
                if(st->el_cache != NULL) mario_free(st->el_cache);
                st->el_cache = nn;
                st->el_cache_cap = ncap;
            }
        }
        if(st->el_cache_len < st->el_cache_cap) {
            st->el_cache[st->el_cache_len].handle = el;
            st->el_cache[st->el_cache_len].obj    = obj;
            st->el_cache_len++;
            js_reanchor_el_cache(vm, st);
        }
    }
    return obj;
}

/* Helper: fetch `this` from the env of a method call. */
static var_t* this_from_env(var_t* env) {
    return get_obj(env, THIS);
}

static var_t* native_el_get_textContent(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    if(st == NULL || el == NULL || st->cb.el_get_text == NULL)
        return var_new_str(vm, "");
    return adopt_cstr(vm, st->cb.el_get_text(st->ctx, el));
}

static var_t* native_el_set_textContent(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    var_t* args = get_func_args(env);
    node_t* n = var_array_get(args, 0);
    const char* s = (n != NULL) ? var_get_str(n->var) : "";
    if(st != NULL && el != NULL && st->cb.el_set_text != NULL) {
        st->cb.el_set_text(st->ctx, el, s ? s : "");
    }
    return NULL;
}

static var_t* native_el_get_innerHTML(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    if(st == NULL || el == NULL || st->cb.el_get_html == NULL)
        return var_new_str(vm, "");
    return adopt_cstr(vm, st->cb.el_get_html(st->ctx, el));
}

static var_t* native_el_set_innerHTML(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    var_t* args = get_func_args(env);
    node_t* n = var_array_get(args, 0);
    const char* s = (n != NULL) ? var_get_str(n->var) : "";
    if(st != NULL && el != NULL && st->cb.el_set_html != NULL) {
        st->cb.el_set_html(st->ctx, el, s ? s : "");
    }
    return NULL;
}

/* Element.id is served by the attribute-property table below (kAttrProps),
 * which also gives it a setter. */

static var_t* native_el_get_tagName(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    if(st == NULL || el == NULL || st->cb.el_get_tag == NULL)
        return var_new_str(vm, "");
    /* HTML parsers fold tag names to lower case internally (litehtml's
     * html_tag::set_tagName does exactly that), but the DOM specifies
     * Element.tagName and Node.nodeName as UPPER case for HTML elements and
     * real pages compare against "DIV"/"A"/"BODY". Normalize here rather than
     * in every embedder: the bridge owns the Web contract. The string is ours
     * to mutate - adopt_cstr below takes ownership of it. */
    char* tag = st->cb.el_get_tag(st->ctx, el);
    if(tag != NULL) {
        for(char* p = tag; *p != 0; ++p) *p = js_ascii_upper(*p);
    }
    return adopt_cstr(vm, tag);
}

/* Node.nodeType. React's createRoot/hydrateRoot validate the container with
 * `node.nodeType` (1 element / 3 text / 9 document); without it every wrapper
 * reads undefined and hydration aborts with "Target container is not a DOM
 * element" (React #299). Elements are 1; the embedder's el_is_tag() tells a
 * text node (3) from a real element, defaulting to element when absent. */
static var_t* native_el_get_nodeType(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    if(st == NULL || el == NULL)
        return var_new_int(vm, 1);
    if(st->cb.el_is_comment != NULL && st->cb.el_is_comment(st->ctx, el))
        return var_new_int(vm, 8); /* COMMENT_NODE */
    if(st->cb.el_is_tag != NULL && !st->cb.el_is_tag(st->ctx, el))
        return var_new_int(vm, 3); /* TEXT_NODE */
    return var_new_int(vm, 1);     /* ELEMENT_NODE */
}

/* Node.nodeValue / CharacterData.data: the text of a text node, null on
 * elements. React's hydration diff reads nodeValue off every hydrated text
 * instance; without it every text node compares undefined against the server
 * string and hydrateRoot bails (Minified React error #423). */
static var_t* native_el_get_nodeValue(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    if(st == NULL || el == NULL)
        return var_new_null(vm);
    if(st->cb.el_is_tag == NULL || st->cb.el_is_tag(st->ctx, el))
        return var_new_null(vm); /* elements: nodeValue is null */
    if(st->cb.el_get_text == NULL)
        return var_new_str(vm, "");
    return adopt_cstr(vm, st->cb.el_get_text(st->ctx, el));
}

static var_t* native_el_set_nodeValue(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    if(st == NULL || el == NULL)
        return NULL;
    if(st->cb.el_is_tag != NULL && st->cb.el_is_tag(st->ctx, el))
        return NULL; /* elements: assignment is a no-op */
    var_t* args = get_func_args(env);
    node_t* n = var_array_get(args, 0);
    const char* s = (n != NULL) ? var_get_str(n->var) : "";
    if(st->cb.el_set_text != NULL)
        st->cb.el_set_text(st->ctx, el, s ? s : "");
    return NULL;
}

static var_t* native_el_getAttribute(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    var_t* args = get_func_args(env);
    node_t* n = var_array_get(args, 0);
    const char* name = (n != NULL) ? var_get_str(n->var) : NULL;
    if(st == NULL || el == NULL || name == NULL || st->cb.el_get_attr == NULL)
        return var_new_null(vm);
    return adopt_cstr(vm, st->cb.el_get_attr(st->ctx, el, name));
}

static var_t* native_el_setAttribute(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    var_t* args = get_func_args(env);
    node_t* nn = var_array_get(args, 0);
    node_t* vn = var_array_get(args, 1);
    const char* name = (nn != NULL) ? var_get_str(nn->var) : NULL;
    const char* value = (vn != NULL) ? var_get_str(vn->var) : "";
    if(st != NULL && el != NULL && name != NULL && st->cb.el_set_attr != NULL) {
        st->cb.el_set_attr(st->ctx, el, name, value ? value : "");
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* window.location.href (getter)                                      */
/* ------------------------------------------------------------------ */

static var_t* native_location_get_href(vm_t* vm, var_t* env, void* data) {
    (void)env;
    js_dom_state* st = state_any(vm, data);
    if(st == NULL || st->cb.get_url == NULL) return var_new_str(vm, "");
    return adopt_cstr(vm, st->cb.get_url(st->ctx));
}

/* ------------------------------------------------------------------ */
/* Date: int64 stamp + wall-clock getters                             */
/*                                                                     */
/* The engine keeps the epoch-ms stamp in a hidden @@t member as a      */
/* 32-bit float, which quantizes to ~131 s steps at Unix-ms magnitude - */
/* useless for a clock's seconds hand. We replace the constructor to    */
/* keep a precise int64 in the instance's ->value (the same slot Element */
/* uses for its borrowed handle) and add getters that break it down via */
/* localtime_r(). getTime()/valueOf() still return a float (the number  */
/* model is 32-bit), but the getters never round-trip through it, so    */
/* they stay exact.                                                    */
/* ------------------------------------------------------------------ */

static int64_t js_now_ms(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_REALTIME, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return (int64_t)time(NULL) * 1000;
}

int64_t js_dom_wall_ms(void) { return js_now_ms(); }

/* The int64 stamp lives inline in ->value (not a heap pointer), so the free
 * hook is a no-op - exactly like Element's borrowed handle. */
static void js_date_free(void* p) { (void)p; }

static int64_t js_date_ms_of(var_t* this_v) {
    if(this_v == NULL) return 0;
    return (int64_t)(intptr_t)this_v->value;
}

static var_t* js_date_ctor(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* obj = this_from_env(env);
    if(obj == NULL) return NULL;
    var_t* arg = get_obj(env, "value");
    int64_t ms = (arg != NULL && arg->type != V_UNDEF)
               ? (int64_t)var_get_float(arg)   /* new Date(ms): best the 32-bit model allows */
               : js_now_ms();                  /* new Date(): precise wall clock */
    obj->value = (void*)(intptr_t)ms;
    obj->free_func = js_date_free;
    return NULL;   /* new_obj keeps `this` when the ctor returns a non-object */
}

static var_t* js_date_get_time(vm_t* vm, var_t* env, void* data) {
    (void)data;
    /* A wall-clock stamp needs more than float32 mantissa (it is ~1.7e12 ms,
     * i.e. beyond 2^24), so the canonical double type keeps it exact. */
    return var_new_float64(vm, (double)js_date_ms_of(this_from_env(env)));
}

/* Date.now(): the static counterpart of new Date().getTime(), and the cheap
 * timestamp pages use to measure elapsed time. */
static var_t* js_date_now(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_float64(vm, (double)js_now_ms());
}

/* field: 0=hours 1=minutes 2=seconds 3=fullYear 4=month(0-based) 5=date 6=day */
static var_t* js_date_field(vm_t* vm, var_t* env, int field) {
    int64_t ms = js_date_ms_of(this_from_env(env));
    time_t sec = (time_t)(ms / 1000);
    struct tm t;
    memset(&t, 0, sizeof(t));
    localtime_r(&sec, &t);
    switch(field) {
        case 0:  return var_new_int(vm, t.tm_hour);
        case 1:  return var_new_int(vm, t.tm_min);
        case 2:  return var_new_int(vm, t.tm_sec);
        case 3:  return var_new_int(vm, t.tm_year + 1900);
        case 4:  return var_new_int(vm, t.tm_mon);        /* JS getMonth is 0-based */
        case 5:  return var_new_int(vm, t.tm_mday);
        default: return var_new_int(vm, t.tm_wday);       /* 0 = Sunday */
    }
}

static var_t* js_date_getHours(vm_t* vm, var_t* env, void* data)    { (void)data; return js_date_field(vm, env, 0); }
static var_t* js_date_getMinutes(vm_t* vm, var_t* env, void* data)  { (void)data; return js_date_field(vm, env, 1); }
static var_t* js_date_getSeconds(vm_t* vm, var_t* env, void* data)  { (void)data; return js_date_field(vm, env, 2); }
static var_t* js_date_getFullYear(vm_t* vm, var_t* env, void* data) { (void)data; return js_date_field(vm, env, 3); }
static var_t* js_date_getMonth(vm_t* vm, var_t* env, void* data)    { (void)data; return js_date_field(vm, env, 4); }
static var_t* js_date_getDate(vm_t* vm, var_t* env, void* data)     { (void)data; return js_date_field(vm, env, 5); }
static var_t* js_date_getDay(vm_t* vm, var_t* env, void* data)      { (void)data; return js_date_field(vm, env, 6); }

/* setTime(ms): replace the stamp, return it (per spec). */
static var_t* js_date_set_time(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* obj = this_from_env(env);
    if(obj == NULL) return NULL;
    var_t* arg = get_obj(env, "ms");
    int64_t ms = (arg != NULL && arg->type != V_UNDEF) ? (int64_t)var_get_float(arg) : 0;
    obj->value = (void*)(intptr_t)ms;
    obj->free_func = js_date_free;
    return var_new_float64(vm, (double)ms);
}

/* "Wed, 21 Oct 2015 07:28:00 GMT" - toGMTString is the legacy alias of
 * toUTCString; both appear in cookie and analytics code. */
static var_t* js_date_to_gmt_string(vm_t* vm, var_t* env, void* data) {
    (void)data;
    int64_t ms = js_date_ms_of(this_from_env(env));
    time_t sec = (time_t)(ms / 1000);
    struct tm t;
    memset(&t, 0, sizeof(t));
    gmtime_r(&sec, &t);
    static const char* wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char buf[40];
    snprintf(buf, sizeof(buf), "%s, %02d %s %d %02d:%02d:%02d GMT",
             wd[t.tm_wday % 7], t.tm_mday, mo[t.tm_mon % 12], t.tm_year + 1900,
             t.tm_hour, t.tm_min, t.tm_sec);
    return var_new_str(vm, buf);
}

/* getTimezoneOffset(): minutes west of UTC (local time is the engine's clock
 * source, so derive the offset from the C library). */
static var_t* js_date_tz_offset(vm_t* vm, var_t* env, void* data) {
    (void)data; (void)env;
    time_t now = time(NULL);
    struct tm lt;
    memset(&lt, 0, sizeof(lt));
    localtime_r(&now, &lt);
#if defined(__APPLE__) || defined(__linux__)
    return var_new_int(vm, -(int)(lt.tm_gmtoff / 60));
#else
    return var_new_int(vm, 0);
#endif
}

/* Fetch the class var straight off vm->root (side-effect free) rather than
 * vm_new_class(), which would re-run do_extends(); vm_reg_* then resolve to
 * the class prototype and var_add replaces the engine's same-named member. */
static void js_patch_date(vm_t* vm) {
    var_t* cls = var_find_own_member_var(vm->root, CLS_DATE);
    if(cls == NULL) return;
    vm_reg_native(vm, cls, "constructor(value)", js_date_ctor, NULL);
    vm_reg_native(vm, cls, "getTime()",    js_date_get_time, NULL);
    vm_reg_native(vm, cls, "valueOf()",    js_date_get_time, NULL);
    vm_reg_native(vm, cls, "getHours()",   js_date_getHours, NULL);
    vm_reg_native(vm, cls, "getMinutes()", js_date_getMinutes, NULL);
    vm_reg_native(vm, cls, "getSeconds()", js_date_getSeconds, NULL);
    vm_reg_native(vm, cls, "getFullYear()",js_date_getFullYear, NULL);
    vm_reg_native(vm, cls, "getMonth()",   js_date_getMonth, NULL);
    vm_reg_native(vm, cls, "getDate()",    js_date_getDate, NULL);
    vm_reg_native(vm, cls, "getDay()",     js_date_getDay, NULL);
    vm_reg_native(vm, cls, "setTime(ms)",       js_date_set_time, NULL);
    vm_reg_native(vm, cls, "toGMTString()",     js_date_to_gmt_string, NULL);
    vm_reg_native(vm, cls, "toUTCString()",     js_date_to_gmt_string, NULL);
    vm_reg_native(vm, cls, "getTimezoneOffset()", js_date_tz_offset, NULL);
    /* Statics live on the class var itself, not its prototype. */
    vm_reg_static(vm, cls, "now()", js_date_now, NULL);
}

/* ------------------------------------------------------------------ */
/* Math: radians-correct sin/cos + numeric PI/E                       */
/*                                                                     */
/* The engine scales sin/cos by (180/PI) (a degree-mode quirk) and      */
/* registers PI/E as zero-arg functions, so `Math.PI * 2` yields a func */
/* object, not a number. Overriding on the prototype fixes both without */
/* touching the submodule.                                             */
/* ------------------------------------------------------------------ */

static var_t* js_math_trig(vm_t* vm, var_t* env, bool is_sin) {
    double a = (double)get_float(env, "a");
    return var_new_float(vm, (float)(is_sin ? sin(a) : cos(a)));
}
static var_t* js_math_sin(vm_t* vm, var_t* env, void* data) { (void)data; return js_math_trig(vm, env, true); }
static var_t* js_math_cos(vm_t* vm, var_t* env, void* data) { (void)data; return js_math_trig(vm, env, false); }

static void js_patch_math(vm_t* vm) {
    var_t* cls = var_find_own_member_var(vm->root, CLS_MATH);
    if(cls == NULL) return;
    /* vm_reg_var installs a plain (non-getter) data member on the prototype,
     * replacing the engine's PI()/E() functions with numbers. */
    vm_reg_var(vm, cls, "PI", var_new_float(vm, 3.14159265358979f), true);
    vm_reg_var(vm, cls, "E",  var_new_float(vm, 2.71828182845905f), true);
    vm_reg_static(vm, cls, "sin(a)", js_math_sin, NULL);
    vm_reg_static(vm, cls, "cos(a)", js_math_cos, NULL);
}

/* ------------------------------------------------------------------ */
/* Timers: setInterval/clearInterval (+ setTimeout/clearTimeout)      */
/*                                                                     */
/* The bridge is clock-agnostic: the embedder drives js_dom_poll_timers */
/* with a monotonic ms clock and we accumulate the delta between calls. */
/* Callbacks must outlive the script run that scheduled them, and the   */
/* GC marks from vm->root ignoring refcounts, so every live callback is */
/* mirrored into a hidden @@timers array on the bridge var.            */
/* ------------------------------------------------------------------ */

/* Rebuild the @@timers anchor array from the current active set. var_add
 * replaces the prior array (node_replace refs the new, unrefs the old); the
 * gc_defer window covers the moment the entries are momentarily unreachable. */
static void js_reanchor_timers(vm_t* vm, js_dom_state* st) {
    var_t* bridge = var_find_own_member_var(vm->root, DOM_BRIDGE_KEY);
    if(bridge == NULL) return;
    vm->gc.gc_defer++;
    var_t* fresh = var_new_array(vm);
    for(int i = 0; i < JS_TIMER_MAX; ++i) {
        if(st->timers[i].active && st->timers[i].cb != NULL)
            var_array_add(fresh, st->timers[i].cb);
    }
    node_t* n = var_add(bridge, DOM_TIMERS_KEY, fresh);
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
    vm->gc.gc_defer--;
}

static int js_add_timer(vm_t* vm, js_dom_state* st, var_t* cb, uint32_t ms, bool repeat) {
    int slot = -1;
    for(int i = 0; i < JS_TIMER_MAX; ++i)
        if(!st->timers[i].active) { slot = i; break; }
    if(slot < 0) return 0;                       /* table full: report failure */
    st->timer_next_id++;
    if(st->timer_next_id <= 0) st->timer_next_id = 1;
    js_timer_t* t = &st->timers[slot];
    t->id           = st->timer_next_id;
    t->cb           = cb;
    t->period_ms    = (ms < 1) ? 1 : ms;
    t->remaining_ms = (int64_t)t->period_ms;
    t->repeat       = repeat;
    t->active       = true;
    js_reanchor_timers(vm, st);
    return t->id;
}

static void js_clear_timer(vm_t* vm, js_dom_state* st, int id) {
    if(id <= 0) return;
    bool changed = false;
    for(int i = 0; i < JS_TIMER_MAX; ++i) {
        if(st->timers[i].active && st->timers[i].id == id) {
            st->timers[i].active = false;
            st->timers[i].cb = NULL;
            changed = true;
        }
    }
    if(changed) js_reanchor_timers(vm, st);
}

static var_t* js_set_timer(vm_t* vm, var_t* env, void* data, bool repeat) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    node_t* cn = var_array_get(args, 0);
    node_t* mn = var_array_get(args, 1);
    if(st == NULL || cn == NULL || cn->var == NULL || !cn->var->is_func)
        return var_new_int(vm, 0);
    uint32_t ms = (mn != NULL && mn->var != NULL) ? (uint32_t)var_get_float(mn->var) : 0;
    return var_new_int(vm, js_add_timer(vm, st, cn->var, ms, repeat));
}

static var_t* js_clear_timer_native(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    node_t* n = var_array_get(args, 0);
    if(st != NULL && n != NULL && n->var != NULL)
        js_clear_timer(vm, st, var_get_int(n->var));
    return NULL;
}

static var_t* native_setInterval(vm_t* vm, var_t* env, void* data)   { return js_set_timer(vm, env, data, true); }
static var_t* native_setTimeout(vm_t* vm, var_t* env, void* data)    { return js_set_timer(vm, env, data, false); }
static var_t* native_clearInterval(vm_t* vm, var_t* env, void* data) { return js_clear_timer_native(vm, env, data); }
static var_t* native_clearTimeout(vm_t* vm, var_t* env, void* data)  { return js_clear_timer_native(vm, env, data); }

int js_dom_poll_timers(vm_t* vm, uint64_t now_ms) {
    js_dom_state* st = state_from_vm(vm);
    if(vm == NULL || st == NULL) return 0;

    if(!st->timer_synced) {                 /* first poll only seeds the clock */
        st->timer_last_now = now_ms;
        st->timer_synced = true;
        return 0;
    }
    if(now_ms <= st->timer_last_now) return 0;   /* nothing elapsed / clock stepped back */
    uint64_t delta = now_ms - st->timer_last_now;
    st->timer_last_now = now_ms;

    /* Phase 1: charge the elapsed time to every active timer exactly once. */
    for(int i = 0; i < JS_TIMER_MAX; ++i)
        if(st->timers[i].active)
            st->timers[i].remaining_ms -= (int64_t)delta;

    /* Phase 2: fire due timers. Re-scan from the top after each fire because a
     * callback may add/clear timers; bounded to avoid a runaway loop. A fired
     * one-shot stays anchored in the OLD @@timers array until the rebuild below,
     * so the GC can not sweep its callback mid-call. */
    int fired = 0;
    for(int guard = 0; guard < 64; ++guard) {
        int due = -1;
        for(int i = 0; i < JS_TIMER_MAX; ++i)
            if(st->timers[i].active && st->timers[i].remaining_ms <= 0) { due = i; break; }
        if(due < 0) break;
        js_timer_t* t = &st->timers[due];
        var_t* cb = t->cb;
        if(t->repeat)
            t->remaining_ms = (int64_t)t->period_ms;   /* one fire per poll, no catch-up burst */
        else {
            t->active = false;
            t->cb = NULL;
        }
        if(cb != NULL && cb->is_func) {
            if(getenv("MARIO_TIMERDBG") != NULL)
                fprintf(stderr, "[timerdbg] fire id=%d cb=%p scope_top=%d stack_top=%d call_depth=%d\n",
                    t->id, (void*)cb, (int)vm->scope_stack_top, (int)vm->stack_top, (int)vm->call_depth);
            var_t* args = var_new_array(vm);
            extern int mario_scopedbg_arm;
            mario_scopedbg_arm = 1;
            var_t* r = call_m_func(vm, NULL, cb, args);
            mario_scopedbg_arm = 0;
            if(r != NULL) var_unref(r);
            var_unref(args);
            fired++;
        }
    }
    js_reanchor_timers(vm, st);   /* drop fired one-shots from the GC anchor */
    return fired;
}

/* ------------------------------------------------------------------ */
/* Shared Element plumbing for the DOM level-1/2 surface              */
/* ------------------------------------------------------------------ */

/* `this` handle of an Element/Style/DOMTokenList method or accessor. */
static js_element_t this_handle(var_t* env) {
    return handle_from_this(this_from_env(env));
}

/* Append an Element wrapper for `el` to `arr`. The engine's own natives
 * (native_Object_keys -> var_array_add(arr, var_new_str(...))) leave the
 * transient creation ref to the GC, so we do the same; callers building a
 * list hold vm->gc.gc_defer for the duration. */
static void arr_add_element(vm_t* vm, var_t* arr, js_element_t el) {
    if(el == NULL || arr == NULL) return;
    var_array_add(arr, wrap_element(vm, el));
}

/* CSS selector query. `root` == NULL searches the whole document, otherwise
 * the subtree of `root` (root itself excluded, as in the DOM). Results are
 * paged through query_all() in JS_DOM_QUERY_CHUNK-sized batches so an
 * unbounded result set still comes back complete without a heap buffer. */
static var_t* query_elements(vm_t* vm, js_dom_state* st, js_element_t root, const char* sel) {
    var_t* arr = var_new_array(vm);
    if(st == NULL || sel == NULL || sel[0] == 0 || st->cb.query_all == NULL) return arr;
    vm->gc.gc_defer++;
    js_element_t buf[JS_DOM_QUERY_CHUNK];
    int skip = 0;
    /* 256 pages * 32 = 8192 elements: far past any real page, and a hard stop
     * in case an embedder keeps reporting a full batch. */
    for(int page = 0; page < 256; ++page) {
        memset(buf, 0, sizeof(buf));
        int n = st->cb.query_all(st->ctx, root, sel, skip, buf, JS_DOM_QUERY_CHUNK);
        if(n <= 0) break;
        if(n > JS_DOM_QUERY_CHUNK) n = JS_DOM_QUERY_CHUNK;
        for(int i = 0; i < n; ++i) arr_add_element(vm, arr, buf[i]);
        if(n < JS_DOM_QUERY_CHUNK) break;
        skip += n;
    }
    vm->gc.gc_defer--;
    return arr;
}

static js_element_t query_first(vm_t* vm, js_dom_state* st, js_element_t root, const char* sel) {
    if(st == NULL || sel == NULL || sel[0] == 0 || st->cb.query_all == NULL) return NULL;
    js_element_t one = NULL;
    int n = st->cb.query_all(st->ctx, root, sel, 0, &one, 1);
    return (n > 0) ? one : NULL;
}

/* children / childNodes share one walk; `tags_only` drops text nodes when the
 * embedder can tell them apart (el_is_tag). */
static var_t* collect_children(vm_t* vm, js_dom_state* st, js_element_t el, bool tags_only) {
    var_t* arr = var_new_array(vm);
    if(st == NULL || el == NULL || st->cb.el_child_count == NULL || st->cb.el_child == NULL)
        return arr;
    vm->gc.gc_defer++;
    int n = st->cb.el_child_count(st->ctx, el);
    for(int i = 0; i < n; ++i) {
        js_element_t c = st->cb.el_child(st->ctx, el, i);
        if(c == NULL) continue;
        if(tags_only && st->cb.el_is_tag != NULL && !st->cb.el_is_tag(st->ctx, c)) continue;
        arr_add_element(vm, arr, c);
    }
    vm->gc.gc_defer--;
    return arr;
}

/* The `idx`-th child, counting only element children when tags_only. */
static js_element_t nth_child(js_dom_state* st, js_element_t el, int idx, bool tags_only) {
    if(st == NULL || el == NULL || idx < 0) return NULL;
    if(st->cb.el_child == NULL) return NULL;
    if(!tags_only) return st->cb.el_child(st->ctx, el, idx);
    int seen = 0;
    int n = (st->cb.el_child_count != NULL) ? st->cb.el_child_count(st->ctx, el) : 0;
    for(int i = 0; i < n; ++i) {
        js_element_t c = st->cb.el_child(st->ctx, el, i);
        if(c == NULL) continue;
        if(st->cb.el_is_tag != NULL && !st->cb.el_is_tag(st->ctx, c)) continue;
        if(seen == idx) return c;
        seen++;
    }
    return NULL;
}

/* Position of `child` among its siblings (-1 when it is not a child). */
static int child_index(js_dom_state* st, js_element_t parent, js_element_t child, bool tags_only) {
    if(st == NULL || parent == NULL || child == NULL) return -1;
    if(st->cb.el_child_count == NULL || st->cb.el_child == NULL) return -1;
    int idx = 0;
    int n = st->cb.el_child_count(st->ctx, parent);
    for(int i = 0; i < n; ++i) {
        js_element_t c = st->cb.el_child(st->ctx, parent, i);
        if(c == NULL) continue;
        bool is_tag = (st->cb.el_is_tag == NULL) || st->cb.el_is_tag(st->ctx, c);
        if(tags_only && !is_tag) continue;
        if(c == child) return idx;
        idx++;
    }
    return -1;
}

/* next/previousSibling(+Element): find the node among the parent's children
 * and step `delta` positions. */
static js_element_t sibling_of(js_dom_state* st, js_element_t el, int delta, bool tags_only) {
    if(st == NULL || el == NULL || delta == 0 || st->cb.el_parent == NULL) return NULL;
    js_element_t p = st->cb.el_parent(st->ctx, el);
    if(p == NULL) return NULL;
    int idx = child_index(st, p, el, tags_only);
    if(idx < 0) return NULL;
    return nth_child(st, p, idx + delta, tags_only);
}

/* Count of element children (childElementCount). */
static int count_children(js_dom_state* st, js_element_t el, bool tags_only) {
    if(st == NULL || el == NULL || st->cb.el_child_count == NULL) return 0;
    int n = st->cb.el_child_count(st->ctx, el);
    if(!tags_only || st->cb.el_is_tag == NULL || st->cb.el_child == NULL) return n;
    int tags = 0;
    for(int i = 0; i < n; ++i) {
        js_element_t c = st->cb.el_child(st->ctx, el, i);
        if(c != NULL && st->cb.el_is_tag(st->ctx, c)) tags++;
    }
    return tags;
}

/* ------------------------------------------------------------------ */
/* document.querySelector / querySelectorAll / getElementsBy*         */
/* ------------------------------------------------------------------ */

static const char* arg_selector(var_t* env, mstr_t* scratch) {
    return js_arg_cstr(env, 0, scratch);
}

static var_t* native_document_querySelector(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* sel = arg_selector(env, s);
    js_element_t el = query_first(vm, st, NULL, sel);
    mstr_free(s);
    if(el == NULL) return var_new_null(vm);
    return wrap_element(vm, el);
}

static var_t* native_document_querySelectorAll(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* sel = arg_selector(env, s);
    var_t* arr = query_elements(vm, st, NULL, sel);
    mstr_free(s);
    return arr;
}

static var_t* native_document_getElementsByTagName(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* tag = arg_selector(env, s);
    /* Tag names are case-insensitive in HTML; the selector engine matches the
     * lower-cased form the parser stored. "*" means every element. */
    mstr_t* sel = mstr_new("");
    if(tag[0] != 0 && strcmp(tag, "*") != 0) {
        for(const char* p = tag; *p != 0; ++p) mstr_add(sel, js_ascii_lower(*p));
    } else {
        mstr_append(sel, "*");
    }
    var_t* arr = query_elements(vm, st, NULL, sel->cstr);
    mstr_free(sel);
    mstr_free(s);
    return arr;
}

static var_t* native_document_get_scripts(vm_t* vm, var_t* env, void* data) {
    /* document.scripts: live collection of every <script> element. Security
     * SDKs (taobao baxia) locate their own tag through it
     * (`document.scripts[i].parentNode.insertBefore(...)`) when currentScript
     * is unavailable; without it the chain dereferences undefined and the SDK
     * never installs its request signer. query_elements returns a plain JS
     * array, which covers length/[i] iteration the way an HTMLCollection would. */
    js_dom_state* st = state_any(vm, data);
    (void)env;
    return query_elements(vm, st, NULL, "script");
}

static var_t* native_document_getElementsByClassName(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* cls = arg_selector(env, s);
    /* "a b" must match elements carrying BOTH classes: build ".a.b". */
    mstr_t* sel = mstr_new("");
    const char* p = cls;
    while(*p != 0) {
        while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if(*p == 0) break;
        mstr_add(sel, '.');
        while(*p != 0 && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            mstr_add(sel, *p);
            p++;
        }
    }
    var_t* arr = (sel->len > 0) ? query_elements(vm, st, NULL, sel->cstr)
                                : var_new_array(vm);
    mstr_free(sel);
    mstr_free(s);
    return arr;
}

static var_t* native_document_getElementsByName(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* name = arg_selector(env, s);
    mstr_t* sel = mstr_new("[name=\"");
    mstr_append(sel, name);
    mstr_add(sel, '"');
    mstr_append(sel, "]");
    var_t* arr = query_elements(vm, st, NULL, sel->cstr);
    mstr_free(sel);
    mstr_free(s);
    return arr;
}

/* ------------------------------------------------------------------ */
/* document.createElement / createTextNode / head / documentElement   */
/* ------------------------------------------------------------------ */

static var_t* native_document_createElement(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* tag = arg_selector(env, s);
    js_element_t el = NULL;
    if(st != NULL && st->cb.create_element != NULL && tag[0] != 0) {
        mstr_t* low = mstr_new("");
        for(const char* p = tag; *p != 0; ++p) mstr_add(low, js_ascii_lower(*p));
        el = st->cb.create_element(st->ctx, low->cstr);
        mstr_free(low);
    }
    mstr_free(s);
    if(el == NULL) return var_new_null(vm);
    return wrap_element(vm, el);
}

static var_t* native_document_createTextNode(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* text = arg_selector(env, s);
    js_element_t el = NULL;
    if(st != NULL && st->cb.create_text_node != NULL)
        el = st->cb.create_text_node(st->ctx, text);
    mstr_free(s);
    if(el == NULL) return var_new_null(vm);
    return wrap_element(vm, el);
}

/* document.createComment(text): React parks Suspense boundary markers
 * (`<!--$-->`, `<!--/$-->`) in the live tree during client rendering; without
 * createComment its render work loop throws and the whole root is discarded,
 * taking every <script> the tree would have mounted with it. */
static var_t* native_document_createComment(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* text = arg_selector(env, s);
    js_element_t el = NULL;
    if(st != NULL && st->cb.create_comment != NULL)
        el = st->cb.create_comment(st->ctx, text);
    mstr_free(s);
    if(el == NULL) return var_new_null(vm);
    return wrap_element(vm, el);
}

/* document.createElementNS(ns, tag): this engine has no XML namespaces, and
 * every namespaced use on the Web is SVG/MathML whose local name is what the
 * layout engine keys off - so fold to a plain createElement of the local name
 * (the part after ':', if the caller passed a qualified name). */
static var_t* native_document_createElementNS(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    node_t* n1 = var_array_get(args, 1);
    const char* tag = (n1 != NULL) ? var_get_str(n1->var) : NULL;
    js_element_t el = NULL;
    if(st != NULL && st->cb.create_element != NULL && tag != NULL && tag[0] != 0) {
        const char* colon = strrchr(tag, ':');
        const char* local = (colon != NULL) ? colon + 1 : tag;
        mstr_t* low = mstr_new("");
        for(const char* p = local; *p != 0; ++p) mstr_add(low, js_ascii_lower(*p));
        el = st->cb.create_element(st->ctx, low->cstr);
        mstr_free(low);
    }
    if(el == NULL) return var_new_null(vm);
    return wrap_element(vm, el);
}

/* One accessor serving document.documentElement / body / head: `data` carries
 * the bridge var as usual, and the index selects which node to fetch. */
static js_element_t doc_node(js_dom_state* st, int which) {
    if(st == NULL) return NULL;
    switch(which) {
        case 0:  /* documentElement */
            return (st->cb.get_root != NULL) ? st->cb.get_root(st->ctx) : NULL;
        case 1:  /* head: no hook, no head - returning <html> here would let
                    document.head.appendChild() corrupt the document. */
            return (st->cb.get_head != NULL) ? st->cb.get_head(st->ctx) : NULL;
        default: /* body: explicit hook, else the historical id lookup */
            if(st->cb.get_body != NULL) return st->cb.get_body(st->ctx);
            if(st->cb.get_element_by_id != NULL)
                return st->cb.get_element_by_id(st->ctx, "body");
            return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Inline style ("style" attribute) editing helpers                   */
/*                                                                    */
/* element.style.* is backed by the element's style attribute, which   */
/* the embedder already knows how to get/set - no extra callback and   */
/* no second source of truth to keep in sync.                          */
/* ------------------------------------------------------------------ */

static bool css_name_eq(const char* a, uint32_t alen, const char* b) {
    if(b == NULL) return false;
    uint32_t blen = (uint32_t)strlen(b);
    if(alen != blen) return false;
    for(uint32_t i = 0; i < alen; ++i)
        if(js_ascii_lower(a[i]) != js_ascii_lower(b[i])) return false;
    return true;
}

static const char* css_skip_space(const char* p, const char* end) {
    while(p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Find `prop` in the CSS declaration list `style`, copying its trimmed value
 * into `out`. Returns false when the property is absent. */
static bool css_find(const char* style, const char* prop, mstr_t* out) {
    if(style == NULL || prop == NULL) return false;
    const char* p = style;
    while(*p != 0) {
        const char* semi = strchr(p, ';');
        const char* end = (semi != NULL) ? semi : p + strlen(p);
        const char* colon = NULL;
        for(const char* q = p; q < end; ++q)
            if(*q == ':') { colon = q; break; }
        if(colon != NULL) {
            const char* ns = css_skip_space(p, colon);
            const char* ne = colon;
            while(ne > ns && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
            if(css_name_eq(ns, (uint32_t)(ne - ns), prop)) {
                const char* vs = css_skip_space(colon + 1, end);
                const char* ve = end;
                while(ve > vs && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r')) ve--;
                mstr_reset(out);
                js_mstr_append(out, vs, (uint32_t)(ve - vs));
                return true;
            }
        }
        p = (semi != NULL) ? semi + 1 : end;
    }
    return false;
}

/* Rebuild `style` with `prop` set to `value` (value == NULL drops it).
 * Returns a mario_malloc'd string the caller frees with mario_free(). */
static char* css_decl_set(const char* style, const char* prop, const char* value) {
    mstr_t* out = mstr_new("");
    bool written = false;
    const char* p = (style != NULL) ? style : "";
    while(*p != 0) {
        const char* semi = strchr(p, ';');
        const char* end = (semi != NULL) ? semi : p + strlen(p);
        const char* colon = NULL;
        for(const char* q = p; q < end; ++q)
            if(*q == ':') { colon = q; break; }
        const char* ns = css_skip_space(p, end);
        bool is_target = false;
        if(colon != NULL) {
            const char* ne = colon;
            while(ne > ns && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
            is_target = css_name_eq(ns, (uint32_t)(ne - ns), prop);
        }
        if(is_target) {
            /* Replace in place (keeps declaration order) or drop it. */
            if(value != NULL) {
                if(out->len > 0) mstr_add(out, ';');
                mstr_append(out, prop);
                mstr_append(out, ": ");
                mstr_append(out, value);
                written = true;
            }
        } else if(ns < end) {
            /* Keep every other declaration verbatim (trimmed of its semicolon). */
            if(out->len > 0) mstr_add(out, ';');
            js_mstr_append(out, ns, (uint32_t)(end - ns));
        }
        p = (semi != NULL) ? semi + 1 : end;
    }
    if(!written && value != NULL) {
        if(out->len > 0) mstr_add(out, ';');
        mstr_append(out, prop);
        mstr_append(out, ": ");
        mstr_append(out, value);
    }
    char* res = js_strdup(out->cstr);
    mstr_free(out);
    return res;
}

/* Number of declarations in a style string (Style.length). */
static int css_decl_count(const char* style) {
    if(style == NULL) return 0;
    int n = 0;
    const char* p = style;
    while(*p != 0) {
        const char* semi = strchr(p, ';');
        const char* end = (semi != NULL) ? semi : p + strlen(p);
        const char* ns = css_skip_space(p, end);
        if(ns < end) {
            for(const char* q = ns; q < end; ++q)
                if(*q == ':') { n++; break; }
        }
        p = (semi != NULL) ? semi + 1 : end;
    }
    return n;
}

/* The `idx`-th property name (CSS dashed form) of a style string. */
static char* css_decl_name(const char* style, int idx) {
    if(style == NULL || idx < 0) return NULL;
    int n = 0;
    const char* p = style;
    while(*p != 0) {
        const char* semi = strchr(p, ';');
        const char* end = (semi != NULL) ? semi : p + strlen(p);
        const char* colon = NULL;
        for(const char* q = p; q < end; ++q)
            if(*q == ':') { colon = q; break; }
        if(colon != NULL) {
            const char* ns = css_skip_space(p, colon);
            const char* ne = colon;
            while(ne > ns && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
            if(ne > ns) {
                if(n == idx) return js_strndup(ns, (uint32_t)(ne - ns));
                n++;
            }
        }
        p = (semi != NULL) ? semi + 1 : end;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* classList (DOMTokenList over the "class" attribute)                */
/* ------------------------------------------------------------------ */

static bool token_eq(const char* a, uint32_t alen, const char* b, uint32_t blen) {
    if(alen != blen) return false;
    for(uint32_t i = 0; i < alen; ++i) if(a[i] != b[i]) return false;
    return true;
}

static bool token_has(const char* list, const char* tok) {
    if(list == NULL || tok == NULL || tok[0] == 0) return false;
    uint32_t tlen = (uint32_t)strlen(tok);
    const char* p = list;
    while(*p != 0) {
        const char* s = css_skip_space(p, p + strlen(p));
        if(*s == 0) break;
        const char* e = s;
        while(*e != 0 && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
        if(token_eq(s, (uint32_t)(e - s), tok, tlen)) return true;
        p = e;
    }
    return false;
}

/* Rebuild the class list with `tok` added / removed. `add` decides which.
 * Returns a mario_malloc'd string (never NULL). */
static char* token_set(const char* list, const char* tok, bool add) {
    mstr_t* out = mstr_new("");
    bool present = false;
    if(list != NULL) {
        const char* p = list;
        while(*p != 0) {
            const char* s = css_skip_space(p, p + strlen(p));
            if(*s == 0) break;
            const char* e = s;
            while(*e != 0 && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
            bool same = token_eq(s, (uint32_t)(e - s), tok, (uint32_t)strlen(tok));
            if(same) present = true;
            if(!same) {
                if(out->len > 0) mstr_add(out, ' ');
                js_mstr_append(out, s, (uint32_t)(e - s));
            }
            p = e;
        }
    }
    if(add && !present) {
        if(out->len > 0) mstr_add(out, ' ');
        mstr_append(out, tok);
    }
    char* res = js_strdup(out->cstr);
    mstr_free(out);
    return (res != NULL) ? res : js_strdup("");
}

/* The `idx`-th token of a whitespace separated list (mario_malloc'd or NULL). */
static char* token_at(const char* list, int idx) {
    if(list == NULL || idx < 0) return NULL;
    int n = 0;
    const char* p = list;
    while(*p != 0) {
        const char* s = css_skip_space(p, p + strlen(p));
        if(*s == 0) break;
        const char* e = s;
        while(*e != 0 && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
        if(n == idx) return js_strndup(s, (uint32_t)(e - s));
        n++;
        p = e;
    }
    return NULL;
}

static int token_count(const char* list) {
    if(list == NULL) return 0;
    int n = 0;
    const char* p = list;
    while(*p != 0) {
        const char* s = css_skip_space(p, p + strlen(p));
        if(*s == 0) break;
        const char* e = s;
        while(*e != 0 && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
        n++;
        p = e;
    }
    return n;
}

/* Read/write the class attribute of `el` through the plain attribute
 * callbacks, so classList and className stay consistent with setAttribute. */
static char* class_attr(js_dom_state* st, js_element_t el) {
    if(st == NULL || el == NULL || st->cb.el_get_attr == NULL) return NULL;
    return st->cb.el_get_attr(st->ctx, el, "class");
}

static void set_class_attr(js_dom_state* st, js_element_t el, const char* value) {
    if(st == NULL || el == NULL || st->cb.el_set_attr == NULL) return;
    st->cb.el_set_attr(st->ctx, el, "class", (value != NULL) ? value : "");
}

/* ------------------------------------------------------------------ */
/* Element.style - CSSStyleDeclaration over the style attribute       */
/* ------------------------------------------------------------------ */

/* camelCase JS property -> CSS property name, for `element.style.color` and
 * friends. Anything not listed still works through getPropertyValue() /
 * setProperty(). The accessor natives receive the table index as `data`. */
typedef struct { const char* js; const char* css; } css_prop_t;
static const css_prop_t kCssProps[] = {
    {"color",              "color"},
    {"background",         "background"},
    {"backgroundColor",    "background-color"},
    {"backgroundImage",    "background-image"},
    {"backgroundPosition", "background-position"},
    {"backgroundRepeat",   "background-repeat"},
    {"backgroundSize",     "background-size"},
    {"fontSize",           "font-size"},
    {"fontFamily",         "font-family"},
    {"fontWeight",         "font-weight"},
    {"fontStyle",          "font-style"},
    {"lineHeight",         "line-height"},
    {"letterSpacing",      "letter-spacing"},
    {"textAlign",          "text-align"},
    {"textDecoration",     "text-decoration"},
    {"textTransform",      "text-transform"},
    {"textIndent",         "text-indent"},
    {"textShadow",         "text-shadow"},
    {"display",            "display"},
    {"visibility",         "visibility"},
    {"opacity",            "opacity"},
    {"overflow",           "overflow"},
    {"zIndex",             "z-index"},
    {"position",           "position"},
    {"top",                "top"},
    {"right",              "right"},
    {"bottom",             "bottom"},
    {"left",               "left"},
    {"float",              "float"},
    {"clear",              "clear"},
    {"width",              "width"},
    {"height",             "height"},
    {"minWidth",           "min-width"},
    {"maxWidth",           "max-width"},
    {"minHeight",          "min-height"},
    {"maxHeight",          "max-height"},
    {"margin",             "margin"},
    {"marginTop",          "margin-top"},
    {"marginRight",        "margin-right"},
    {"marginBottom",       "margin-bottom"},
    {"marginLeft",         "margin-left"},
    {"padding",            "padding"},
    {"paddingTop",         "padding-top"},
    {"paddingRight",       "padding-right"},
    {"paddingBottom",      "padding-bottom"},
    {"paddingLeft",        "padding-left"},
    {"border",             "border"},
    {"borderTop",          "border-top"},
    {"borderRight",        "border-right"},
    {"borderBottom",       "border-bottom"},
    {"borderLeft",         "border-left"},
    {"borderWidth",        "border-width"},
    {"borderColor",        "border-color"},
    {"borderStyle",        "border-style"},
    {"borderRadius",       "border-radius"},
    {"boxShadow",          "box-shadow"},
    {"cursor",             "cursor"},
    {"verticalAlign",      "vertical-align"},
    {"whiteSpace",         "white-space"},
    {"wordSpacing",        "word-spacing"},
    {"listStyle",          "list-style"},
    {"listStyleType",      "list-style-type"},
};
#define CSS_PROP_COUNT ((int)(sizeof(kCssProps) / sizeof(kCssProps[0])))

static var_t* wrap_style(vm_t* vm, js_element_t el) {
    var_t* o = new_obj(vm, CLS_STYLE, 0);
    if(o == NULL) return var_new_null(vm);
    o->value = el;
    o->free_func = el_free;
    return o;
}

static var_t* wrap_tokens(vm_t* vm, js_element_t el) {
    var_t* o = new_obj(vm, CLS_TOKENS, 0);
    if(o == NULL) return var_new_null(vm);
    o->value = el;
    o->free_func = el_free;
    return o;
}

/* Inline style first, computed style as the fallback: a page reading
 * el.style.color right after setting it must see its own value even when the
 * embedder has no computed-style hook. */
static char* style_lookup(js_dom_state* st, js_element_t el, const char* css) {
    mstr_t* val = mstr_new("");
    if(st != NULL && el != NULL && css != NULL) {
        if(st->cb.el_get_attr != NULL) {
            char* attr = st->cb.el_get_attr(st->ctx, el, "style");
            if(attr != NULL) {
                css_find(attr, css, val);
                mario_free(attr);
            }
        }
        if(val->len == 0 && st->cb.el_get_style != NULL) {
            char* comp = st->cb.el_get_style(st->ctx, el, css);
            if(comp != NULL) {
                mstr_cpy(val, comp);
                mario_free(comp);
            }
        }
    }
    char* out = js_strdup(val->cstr);
    mstr_free(val);
    return out;
}

/* Rewrite the element's style attribute with `css` set to `value`
 * (NULL/empty removes the declaration). */
static void style_write(js_dom_state* st, js_element_t el, const char* css, const char* value) {
    if(st == NULL || el == NULL || css == NULL || st->cb.el_set_attr == NULL) return;
    char* old = (st->cb.el_get_attr != NULL) ? st->cb.el_get_attr(st->ctx, el, "style") : NULL;
    char* neu = css_decl_set(old, css, (value != NULL && value[0] != 0) ? value : NULL);
    if(old != NULL) mario_free(old);
    st->cb.el_set_attr(st->ctx, el, "style", (neu != NULL) ? neu : "");
    if(neu != NULL) mario_free(neu);
}

static var_t* style_prop_get(vm_t* vm, var_t* env, void* data) {
    int idx = (int)(intptr_t)data;
    if(idx < 0 || idx >= CSS_PROP_COUNT) return var_new_str(vm, "");
    js_dom_state* st = state_from_vm(vm);
    char* v = style_lookup(st, this_handle(env), kCssProps[idx].css);
    var_t* r = var_new_str(vm, (v != NULL) ? v : "");
    if(v != NULL) mario_free(v);
    return r;
}

static var_t* style_prop_set(vm_t* vm, var_t* env, void* data) {
    int idx = (int)(intptr_t)data;
    if(idx < 0 || idx >= CSS_PROP_COUNT) return NULL;
    js_dom_state* st = state_from_vm(vm);
    mstr_t* s = mstr_new("");
    const char* value = js_arg_cstr(env, 0, s);
    style_write(st, this_handle(env), kCssProps[idx].css, value);
    mstr_free(s);
    return NULL;
}

static var_t* style_get_cssText(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_get_attr == NULL) return var_new_str(vm, "");
    return adopt_cstr(vm, st->cb.el_get_attr(st->ctx, el, "style"));
}

static var_t* style_set_cssText(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_set_attr == NULL) return NULL;
    mstr_t* s = mstr_new("");
    const char* value = js_arg_cstr(env, 0, s);
    st->cb.el_set_attr(st->ctx, el, "style", value);
    mstr_free(s);
    return NULL;
}

static var_t* style_getPropertyValue(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* prop = js_arg_cstr(env, 0, s);
    char* v = style_lookup(st, this_handle(env), prop);
    var_t* r = var_new_str(vm, (v != NULL) ? v : "");
    if(v != NULL) mario_free(v);
    mstr_free(s);
    return r;
}

static var_t* style_setProperty(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    mstr_t* t = mstr_new("");
    const char* prop = js_arg_cstr(env, 0, s);
    const char* value = js_arg_cstr(env, 1, t);
    style_write(st, this_handle(env), prop, value);
    mstr_free(t);
    mstr_free(s);
    return NULL;
}

static var_t* style_removeProperty(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    mstr_t* s = mstr_new("");
    const char* prop = js_arg_cstr(env, 0, s);
    char* old = NULL;
    if(st != NULL && el != NULL && st->cb.el_get_attr != NULL)
        old = st->cb.el_get_attr(st->ctx, el, "style");
    mstr_t* prev = mstr_new("");
    if(old != NULL) css_find(old, prop, prev);
    style_write(st, el, prop, NULL);
    if(old != NULL) mario_free(old);
    var_t* r = var_new_str(vm, prev->cstr);
    mstr_free(prev);
    mstr_free(s);
    return r;
}

static var_t* style_get_length(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    int n = 0;
    if(st != NULL && el != NULL && st->cb.el_get_attr != NULL) {
        char* attr = st->cb.el_get_attr(st->ctx, el, "style");
        if(attr != NULL) { n = css_decl_count(attr); mario_free(attr); }
    }
    return var_new_int(vm, n);
}

static var_t* style_item(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    int idx = js_arg_int(env, 0);
    char* name = NULL;
    if(st != NULL && el != NULL && st->cb.el_get_attr != NULL) {
        char* attr = st->cb.el_get_attr(st->ctx, el, "style");
        if(attr != NULL) { name = css_decl_name(attr, idx); mario_free(attr); }
    }
    var_t* r = var_new_str(vm, (name != NULL) ? name : "");
    if(name != NULL) mario_free(name);
    return r;
}

/* ------------------------------------------------------------------ */
/* Element.classList - DOMTokenList over the class attribute          */
/* ------------------------------------------------------------------ */

static var_t* tokens_add(vm_t* vm, var_t* env, void* data, bool add) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL) return NULL;
    int n = js_arg_count(env);
    char* cur = class_attr(st, el);
    for(int i = 0; i < n; ++i) {
        mstr_t* s = mstr_new("");
        const char* tok = js_arg_cstr(env, i, s);
        if(tok[0] != 0) {
            char* next = token_set(cur, tok, add);
            if(cur != NULL) mario_free(cur);
            cur = next;
        }
        mstr_free(s);
    }
    set_class_attr(st, el, (cur != NULL) ? cur : "");
    if(cur != NULL) mario_free(cur);
    return NULL;
}

static var_t* native_classList_add(vm_t* vm, var_t* env, void* data)    { return tokens_add(vm, env, data, true); }
static var_t* native_classList_remove(vm_t* vm, var_t* env, void* data) { return tokens_add(vm, env, data, false); }

static var_t* native_classList_contains(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* tok = js_arg_cstr(env, 0, s);
    char* cur = class_attr(st, this_handle(env));
    bool has = token_has(cur, tok);
    if(cur != NULL) mario_free(cur);
    mstr_free(s);
    return var_new_bool(vm, has);
}

/* toggle(token[, force]): returns the resulting state. */
static var_t* native_classList_toggle(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL) return var_new_bool(vm, false);
    mstr_t* s = mstr_new("");
    const char* tok = js_arg_cstr(env, 0, s);
    if(tok[0] == 0) { mstr_free(s); return var_new_bool(vm, false); }
    char* cur = class_attr(st, el);
    bool has = token_has(cur, tok);
    bool want;
    var_t* force_v = js_arg(env, 1);
    /* WebIDL: an optional argument passed as undefined counts as absent. This
     * also absorbs the phantom trailing undefined argument that nested native
     * calls inside event callbacks observe from the VM call convention. */
    if(force_v != NULL && force_v->type != V_UNDEF) want = js_truthy(force_v);
    else                                            want = !has;
    if(want != has) {
        char* next = token_set(cur, tok, want);
        set_class_attr(st, el, next);
        mario_free(next);
    }
    if(cur != NULL) mario_free(cur);
    mstr_free(s);
    return var_new_bool(vm, want);
}

static var_t* native_classList_replace(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL) return var_new_bool(vm, false);
    mstr_t* a = mstr_new("");
    mstr_t* b = mstr_new("");
    const char* old_tok = js_arg_cstr(env, 0, a);
    const char* new_tok = js_arg_cstr(env, 1, b);
    char* cur = class_attr(st, el);
    bool had = token_has(cur, old_tok);
    if(had && new_tok[0] != 0) {
        char* removed = token_set(cur, old_tok, false);
        char* added = token_set(removed, new_tok, true);
        set_class_attr(st, el, added);
        mario_free(removed);
        mario_free(added);
    }
    if(cur != NULL) mario_free(cur);
    mstr_free(b);
    mstr_free(a);
    return var_new_bool(vm, had);
}

static var_t* native_classList_item(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    int idx = js_arg_int(env, 0);
    char* cur = class_attr(st, this_handle(env));
    char* tok = token_at(cur, idx);
    if(cur != NULL) mario_free(cur);
    if(tok == NULL) return var_new_null(vm);
    var_t* r = var_new_str(vm, tok);
    mario_free(tok);
    return r;
}

static var_t* native_classList_get_length(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    char* cur = class_attr(st, this_handle(env));
    int n = token_count(cur);
    if(cur != NULL) mario_free(cur);
    return var_new_int(vm, n);
}

static var_t* native_classList_get_value(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    return adopt_cstr(vm, class_attr(st, this_handle(env)));
}

static var_t* native_classList_set_value(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* v = js_arg_cstr(env, 0, s);
    set_class_attr(st, this_handle(env), v);
    mstr_free(s);
    return NULL;
}

static var_t* native_classList_toString(vm_t* vm, var_t* env, void* data) {
    return native_classList_get_value(vm, env, data);
}

/* ------------------------------------------------------------------ */
/* Element: attributes reflected as properties                        */
/* ------------------------------------------------------------------ */

/* String properties that are a straight reflection of an attribute. The
 * accessor natives receive the table index as `data`. */
typedef struct { const char* js; const char* attr; } attr_prop_t;
static const attr_prop_t kAttrProps[] = {
    {"id",          "id"},
    {"src",         "src"},
    {"href",        "href"},
    {"alt",         "alt"},
    {"title",       "title"},
    {"type",        "type"},
    {"name",        "name"},
    {"value",       "value"},
    {"placeholder", "placeholder"},
    {"htmlFor",     "for"},
    {"width",       "width"},
    {"height",      "height"},
    {"rel",         "rel"},
    {"action",      "action"},
    {"method",      "method"},
    {"lang",        "lang"},
};
#define ATTR_PROP_COUNT ((int)(sizeof(kAttrProps) / sizeof(kAttrProps[0])))

/* Boolean ("present means true") properties and the attribute behind them. */
typedef struct { const char* js; const char* attr; } bool_prop_t;
static const bool_prop_t kBoolProps[] = {
    {"checked",  "checked"},
    {"disabled", "disabled"},
    {"readOnly", "readonly"},
    {"selected", "selected"},
    {"multiple", "multiple"},
    {"hidden",   "hidden"},
};
#define BOOL_PROP_COUNT ((int)(sizeof(kBoolProps) / sizeof(kBoolProps[0])))

static var_t* attr_prop_get(vm_t* vm, var_t* env, void* data) {
    int idx = (int)(intptr_t)data;
    if(idx < 0 || idx >= ATTR_PROP_COUNT) return var_new_str(vm, "");
    js_dom_state* st = state_from_vm(vm);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_get_attr == NULL) return var_new_str(vm, "");
    char* v = st->cb.el_get_attr(st->ctx, el, kAttrProps[idx].attr);
    var_t* r = var_new_str(vm, (v != NULL) ? v : "");
    if(v != NULL) mario_free(v);
    return r;
}

static var_t* attr_prop_set(vm_t* vm, var_t* env, void* data) {
    int idx = (int)(intptr_t)data;
    if(idx < 0 || idx >= ATTR_PROP_COUNT) return NULL;
    js_dom_state* st = state_from_vm(vm);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_set_attr == NULL) return NULL;
    mstr_t* s = mstr_new("");
    const char* v = js_arg_cstr(env, 0, s);
    st->cb.el_set_attr(st->ctx, el, kAttrProps[idx].attr, v);
    mstr_free(s);
    return NULL;
}

static var_t* bool_prop_get(vm_t* vm, var_t* env, void* data) {
    int idx = (int)(intptr_t)data;
    if(idx < 0 || idx >= BOOL_PROP_COUNT) return var_new_bool(vm, false);
    js_dom_state* st = state_from_vm(vm);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_get_attr == NULL) return var_new_bool(vm, false);
    char* v = st->cb.el_get_attr(st->ctx, el, kBoolProps[idx].attr);
    /* HTML boolean attributes are true by presence; "false" is the one value
     * the DOM treats as absent (it is how a script turns one off). */
    bool on = (v != NULL) && (v[0] == 0 || strcmp(v, "false") != 0);
    if(v != NULL) mario_free(v);
    return var_new_bool(vm, on);
}

static var_t* bool_prop_set(vm_t* vm, var_t* env, void* data) {
    int idx = (int)(intptr_t)data;
    if(idx < 0 || idx >= BOOL_PROP_COUNT) return NULL;
    js_dom_state* st = state_from_vm(vm);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL) return NULL;
    bool on = js_truthy(js_arg(env, 0));
    const char* attr = kBoolProps[idx].attr;
    if(on) {
        if(st->cb.el_set_attr != NULL) st->cb.el_set_attr(st->ctx, el, attr, attr);
    } else if(st->cb.el_remove_attr != NULL) {
        st->cb.el_remove_attr(st->ctx, el, attr);
    } else if(st->cb.el_set_attr != NULL) {
        st->cb.el_set_attr(st->ctx, el, attr, "false");
    }
    return NULL;
}

static var_t* native_el_hasAttribute(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    bool has = false;
    if(st != NULL && el != NULL && st->cb.el_get_attr != NULL && name[0] != 0) {
        char* v = st->cb.el_get_attr(st->ctx, el, name);
        has = (v != NULL);
        if(v != NULL) mario_free(v);
    }
    mstr_free(s);
    return var_new_bool(vm, has);
}

static var_t* native_el_removeAttribute(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    if(st != NULL && el != NULL && name[0] != 0 && st->cb.el_remove_attr != NULL)
        st->cb.el_remove_attr(st->ctx, el, name);
    mstr_free(s);
    return NULL;
}

static var_t* native_el_get_className(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    char* v = class_attr(st, this_handle(env));
    var_t* r = var_new_str(vm, (v != NULL) ? v : "");
    if(v != NULL) mario_free(v);
    return r;
}

static var_t* native_el_set_className(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* v = js_arg_cstr(env, 0, s);
    set_class_attr(st, this_handle(env), v);
    mstr_free(s);
    return NULL;
}

static var_t* native_el_get_classList(vm_t* vm, var_t* env, void* data) {
    (void)data;
    return wrap_tokens(vm, this_handle(env));
}

static var_t* native_el_get_style(vm_t* vm, var_t* env, void* data) {
    (void)data;
    return wrap_style(vm, this_handle(env));
}

/* ------------------------------------------------------------------ */
/* Element: tree walking                                              */
/* ------------------------------------------------------------------ */

static js_element_t element_arg(vm_t* vm, var_t* v) {
    if(v == NULL || v->type != V_OBJECT || v->is_func) return NULL;
    /* Same wrapper-identity guard as handle_from_this(): a genuine Element
     * wrapper is exactly a var whose value is freed by el_free. `instanceof
     * Element` alone is not enough - Object.create(Element.prototype), a user
     * subclass, or a recycled var all pass it while carrying a ->value that is
     * a mario heap pointer rather than an embedder element handle. */
    if(v->free_func != el_free) return NULL;
    var_t* cls = var_find_own_member_var(vm->root, CLS_ELEMENT);
    if(cls != NULL && !var_instanceof(v, cls)) return NULL;
    js_element_t el = (js_element_t)v->value;
    if(el == NULL) return NULL;
    /* Liveness gate: see handle_from_this(). A genuine wrapper whose element was
     * freed and recycled must not be handed to a mutation callback. */
    js_dom_state* st = state_from_vm(vm);
    if(st != NULL && st->cb.el_is_live != NULL && !st->cb.el_is_live(st->ctx, el))
        return NULL;
    return el;
}

static var_t* wrap_or_null(vm_t* vm, js_element_t el) {
    return (el == NULL) ? var_new_null(vm) : wrap_element(vm, el);
}

static var_t* native_el_get_children(vm_t* vm, var_t* env, void* data) {
    return collect_children(vm, state_any(vm, data), this_handle(env), true);
}

static var_t* native_el_get_childNodes(vm_t* vm, var_t* env, void* data) {
    return collect_children(vm, state_any(vm, data), this_handle(env), false);
}

static var_t* native_el_get_childElementCount(vm_t* vm, var_t* env, void* data) {
    return var_new_int(vm, count_children(state_any(vm, data), this_handle(env), true));
}

static var_t* native_el_get_parentNode(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_parent == NULL) return var_new_null(vm);
    return wrap_or_null(vm, st->cb.el_parent(st->ctx, el));
}

/* Node.ownerDocument: React-DOM resolves the event-listening root through
 * rootContainer.ownerDocument and calls addEventListener on the result; with
 * the back-pointer missing, createRoot() throws "undefined is not an object"
 * inside its (swallowed) bootstrap and the whole app never mounts (taobao). */
static var_t* native_el_get_ownerDocument(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    var_t* doc = var_find_member_var(vm->root, "document");
    if(doc == NULL) return var_new_null(vm);
    var_ref(doc);
    return doc;
}

/* Node.namespaceURI: HTML elements live in the XHTML namespace; React reads it
 * to tell SVG/MathML hosts apart. undefined made those checks misfire. */
static var_t* native_el_get_namespaceURI(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_str(vm, "http://www.w3.org/1999/xhtml");
}

/* Edge of a child list: `first` picks index 0, else the last one. */
static var_t* edge_child(vm_t* vm, var_t* env, void* data, bool first, bool tags_only) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL) return var_new_null(vm);
    if(first) return wrap_or_null(vm, nth_child(st, el, 0, tags_only));
    int n = count_children(st, el, tags_only);
    return wrap_or_null(vm, (n > 0) ? nth_child(st, el, n - 1, tags_only) : NULL);
}

static var_t* native_el_get_firstChild(vm_t* vm, var_t* env, void* data)        { return edge_child(vm, env, data, true,  false); }
static var_t* native_el_get_lastChild(vm_t* vm, var_t* env, void* data)         { return edge_child(vm, env, data, false, false); }
static var_t* native_el_get_firstElementChild(vm_t* vm, var_t* env, void* data) { return edge_child(vm, env, data, true,  true); }
static var_t* native_el_get_lastElementChild(vm_t* vm, var_t* env, void* data)  { return edge_child(vm, env, data, false, true); }

static var_t* native_el_get_nextSibling(vm_t* vm, var_t* env, void* data) {
    return wrap_or_null(vm, sibling_of(state_any(vm, data), this_handle(env), 1, false));
}
static var_t* native_el_get_previousSibling(vm_t* vm, var_t* env, void* data) {
    return wrap_or_null(vm, sibling_of(state_any(vm, data), this_handle(env), -1, false));
}
static var_t* native_el_get_nextElementSibling(vm_t* vm, var_t* env, void* data) {
    return wrap_or_null(vm, sibling_of(state_any(vm, data), this_handle(env), 1, true));
}
static var_t* native_el_get_previousElementSibling(vm_t* vm, var_t* env, void* data) {
    return wrap_or_null(vm, sibling_of(state_any(vm, data), this_handle(env), -1, true));
}

/* childIndex: position among element siblings (0-based), -1 when detached.
 * Not standard DOM, but handy for pages that need to know "which row am I". */
static var_t* native_el_get_index(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_parent == NULL) return var_new_int(vm, -1);
    js_element_t p = st->cb.el_parent(st->ctx, el);
    if(p == NULL) return var_new_int(vm, -1);
    return var_new_int(vm, child_index(st, p, el, true));
}

/* ------------------------------------------------------------------ */
/* Element: mutation                                                  */
/* ------------------------------------------------------------------ */

static var_t* native_el_appendChild(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    var_t* child = js_arg(env, 0);
    js_element_t ch = element_arg(vm, child);
    if(st == NULL || el == NULL || ch == NULL || st->cb.el_append_child == NULL)
        return var_new_null(vm);
    if(!st->cb.el_append_child(st->ctx, el, ch)) return var_new_null(vm);
    return (child != NULL) ? child : var_new_null(vm);
}

/* ParentNode.append(...nodes): appendChild per argument, variadic. The VM
 * keeps every caller argument in the env's arg array even past the single
 * declared parameter, so js_arg(env, i) reaches them all. String arguments
 * become text nodes per the DOM spec; unknown arguments are skipped. Returns
 * undefined, unlike appendChild which hands the child back. */
static var_t* native_el_append(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_append_child == NULL)
        return var_new_null(vm);
    uint32_t n = get_func_args_num(env);
    for(uint32_t i = 0; i < n; i++) {
        var_t* a = js_arg(env, (int)i);
        js_element_t ch = element_arg(vm, a);
        if(ch == NULL && a != NULL && a->type == V_STRING &&
           st->cb.create_text_node != NULL) {
            mstr_t* tmp = mstr_new("");
            ch = st->cb.create_text_node(st->ctx, js_cstr(a, tmp));
            mstr_free(tmp);
        }
        if(ch == NULL) continue;
        st->cb.el_append_child(st->ctx, el, ch);
    }
    return var_new_null(vm);
}

static var_t* native_el_insertBefore(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    var_t* child = js_arg(env, 0);
    js_element_t ch = element_arg(vm, child);
    js_element_t ref = element_arg(vm, js_arg(env, 1));   /* NULL => append */
    if(st == NULL || el == NULL || ch == NULL || st->cb.el_insert_before == NULL)
        return var_new_null(vm);
    if(!st->cb.el_insert_before(st->ctx, el, ch, ref)) return var_new_null(vm);
    return (child != NULL) ? child : var_new_null(vm);
}

static var_t* native_el_removeChild(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    var_t* child = js_arg(env, 0);
    js_element_t ch = element_arg(vm, child);
    if(st == NULL || el == NULL || ch == NULL || st->cb.el_remove_child == NULL)
        return var_new_null(vm);
    if(!st->cb.el_remove_child(st->ctx, el, ch)) return var_new_null(vm);
    return (child != NULL) ? child : var_new_null(vm);
}

static var_t* native_el_replaceChild(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    var_t* neu = js_arg(env, 0);
    js_element_t nn = element_arg(vm, neu);
    js_element_t old = element_arg(vm, js_arg(env, 1));
    if(st == NULL || el == NULL || nn == NULL || old == NULL) return var_new_null(vm);
    if(st->cb.el_insert_before == NULL || st->cb.el_remove_child == NULL)
        return var_new_null(vm);
    if(!st->cb.el_insert_before(st->ctx, el, nn, old)) return var_new_null(vm);
    st->cb.el_remove_child(st->ctx, el, old);
    return wrap_or_null(vm, old);
}

static var_t* native_el_cloneNode(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_clone_node == NULL)
        return var_new_null(vm);
    /* cloneNode(deep): `deep` defaults to false per the DOM spec, so only an
     * explicitly truthy first argument copies the subtree. */
    int deep = js_truthy(js_arg(env, 0)) ? 1 : 0;
    js_element_t cp = st->cb.el_clone_node(st->ctx, el, deep);
    if(cp == NULL) return var_new_null(vm);
    return wrap_element(vm, cp);
}

static var_t* native_el_remove(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_parent == NULL || st->cb.el_remove_child == NULL)
        return NULL;
    js_element_t p = st->cb.el_parent(st->ctx, el);
    if(p != NULL) st->cb.el_remove_child(st->ctx, p, el);
    return NULL;
}

static var_t* native_el_contains(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    js_element_t other = element_arg(vm, js_arg(env, 0));
    if(st == NULL || el == NULL || other == NULL) return var_new_bool(vm, false);
    if(el == other) return var_new_bool(vm, true);   /* DOM: a node contains itself */
    if(st->cb.el_parent == NULL) return var_new_bool(vm, false);
    /* Walk up from `other`; bounded so a corrupt tree cannot spin forever. */
    js_element_t p = st->cb.el_parent(st->ctx, other);
    for(int guard = 0; p != NULL && guard < 4096; ++guard) {
        if(p == el) return var_new_bool(vm, true);
        p = st->cb.el_parent(st->ctx, p);
    }
    return var_new_bool(vm, false);
}

/* ------------------------------------------------------------------ */
/* Element: scoped queries                                            */
/* ------------------------------------------------------------------ */

static var_t* native_el_querySelector(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* sel = js_arg_cstr(env, 0, s);
    js_element_t el = query_first(vm, st, this_handle(env), sel);
    mstr_free(s);
    return wrap_or_null(vm, el);
}

static var_t* native_el_querySelectorAll(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* sel = js_arg_cstr(env, 0, s);
    var_t* arr = query_elements(vm, st, this_handle(env), sel);
    mstr_free(s);
    return arr;
}

static var_t* native_el_getElementsByTagName(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* tag = js_arg_cstr(env, 0, s);
    mstr_t* sel = mstr_new("");
    if(tag[0] != 0 && strcmp(tag, "*") != 0) {
        for(const char* p = tag; *p != 0; ++p) mstr_add(sel, js_ascii_lower(*p));
    } else {
        mstr_append(sel, "*");
    }
    var_t* arr = query_elements(vm, st, this_handle(env), sel->cstr);
    mstr_free(sel);
    mstr_free(s);
    return arr;
}

static var_t* native_el_getElementsByClassName(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* cls = js_arg_cstr(env, 0, s);
    mstr_t* sel = mstr_new("");
    const char* p = cls;
    while(*p != 0) {
        while(*p == ' ' || *p == '\t') p++;
        if(*p == 0) break;
        mstr_add(sel, '.');
        while(*p != 0 && *p != ' ' && *p != '\t') { mstr_add(sel, *p); p++; }
    }
    var_t* arr = (sel->len > 0) ? query_elements(vm, st, this_handle(env), sel->cstr)
                                : var_new_array(vm);
    mstr_free(sel);
    mstr_free(s);
    return arr;
}

/* Is `el` in the result set of `sel` queried from `root`? The bridge has no
 * "matches" callback, so it answers through the query callback - correct for
 * every selector the engine supports, at the cost of one subtree query. */
static bool matches_selector(vm_t* vm, js_dom_state* st, js_element_t el,
                             js_element_t root, const char* sel) {
    if(st == NULL || el == NULL || sel == NULL || sel[0] == 0) return false;
    js_element_t buf[JS_DOM_QUERY_CHUNK];
    int skip = 0;
    for(int page = 0; page < 64; ++page) {
        memset(buf, 0, sizeof(buf));
        int n = st->cb.query_all != NULL
              ? st->cb.query_all(st->ctx, root, sel, skip, buf, JS_DOM_QUERY_CHUNK) : 0;
        if(n <= 0) return false;
        if(n > JS_DOM_QUERY_CHUNK) n = JS_DOM_QUERY_CHUNK;
        for(int i = 0; i < n; ++i)
            if(buf[i] == el) return true;
        if(n < JS_DOM_QUERY_CHUNK) return false;
        skip += n;
    }
    return false;
}

static var_t* native_el_matches(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    mstr_t* s = mstr_new("");
    const char* sel = js_arg_cstr(env, 0, s);
    js_element_t root = NULL;
    if(st != NULL && el != NULL && st->cb.el_parent != NULL)
        root = st->cb.el_parent(st->ctx, el);   /* NULL root => whole document */
    bool m = matches_selector(vm, st, el, root, sel);
    mstr_free(s);
    return var_new_bool(vm, m);
}

/* closest(sel): the nearest ancestor (self included) matching `sel`. One
 * document-wide query feeds the whole walk, so the cost is a single query
 * rather than one per ancestor. */
static var_t* native_el_closest(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    mstr_t* s = mstr_new("");
    const char* sel = js_arg_cstr(env, 0, s);
    js_element_t hit = NULL;
    if(st != NULL && el != NULL && sel[0] != 0 && st->cb.query_all != NULL) {
        js_element_t set[JS_DOM_QUERY_CHUNK * 4];
        int total = 0;
        int skip = 0;
        for(int page = 0; page < 4 && total < (int)sizeof(set) / (int)sizeof(set[0]); ++page) {
            int n = st->cb.query_all(st->ctx, NULL, sel, skip, set + total,
                                     JS_DOM_QUERY_CHUNK);
            if(n <= 0) break;
            if(n > JS_DOM_QUERY_CHUNK) n = JS_DOM_QUERY_CHUNK;
            total += n;
            if(n < JS_DOM_QUERY_CHUNK) break;
            skip += n;
        }
        for(js_element_t p = el; p != NULL && hit == NULL;) {
            for(int i = 0; i < total; ++i)
                if(set[i] == p) { hit = p; break; }
            if(hit != NULL || st->cb.el_parent == NULL) break;
            p = st->cb.el_parent(st->ctx, p);
        }
    }
    mstr_free(s);
    return wrap_or_null(vm, hit);
}

/* ------------------------------------------------------------------ */
/* Element: geometry / focus                                          */
/* ------------------------------------------------------------------ */

static bool el_rect(js_dom_state* st, js_element_t el, int* x, int* y, int* w, int* h) {
    int lx = 0, ly = 0, lw = 0, lh = 0;
    if(st == NULL || el == NULL || st->cb.el_get_rect == NULL) return false;
    st->cb.el_get_rect(st->ctx, el, &lx, &ly, &lw, &lh);
    if(x != NULL) *x = lx;
    if(y != NULL) *y = ly;
    if(w != NULL) *w = lw;
    if(h != NULL) *h = lh;
    return true;
}

static var_t* native_el_getBoundingClientRect(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    int x = 0, y = 0, w = 0, h = 0;
    el_rect(st, this_handle(env), &x, &y, &w, &h);
    /* A plain object (not a class instance): DOMRect fields are read-only
     * numbers and pages only ever read them. */
    var_t* r = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(r, "x",      var_new_float(vm, (float)x));
    var_add(r, "y",      var_new_float(vm, (float)y));
    var_add(r, "left",   var_new_float(vm, (float)x));
    var_add(r, "top",    var_new_float(vm, (float)y));
    var_add(r, "width",  var_new_float(vm, (float)w));
    var_add(r, "height", var_new_float(vm, (float)h));
    var_add(r, "right",  var_new_float(vm, (float)(x + w)));
    var_add(r, "bottom", var_new_float(vm, (float)(y + h)));
    return r;
}

/* The offset/client/scroll size family all comes from the one border box the
 * embedder can report. `data` selects the field:
 * 0=offsetWidth 1=offsetHeight 2=offsetLeft 3=offsetTop
 * 4=clientWidth 5=clientHeight 6=scrollWidth 7=scrollHeight. */
static var_t* geom_get(vm_t* vm, var_t* env, void* data) {
    int which = (int)(intptr_t)data;
    js_dom_state* st = state_from_vm(vm);
    int x = 0, y = 0, w = 0, h = 0;
    el_rect(st, this_handle(env), &x, &y, &w, &h);
    switch(which) {
        case 0:  return var_new_int(vm, w);
        case 1:  return var_new_int(vm, h);
        case 2:  return var_new_int(vm, x);
        case 3:  return var_new_int(vm, y);
        case 4:  return var_new_int(vm, w);
        case 5:  return var_new_int(vm, h);
        case 6:  return var_new_int(vm, w);
        default: return var_new_int(vm, h);
    }
}

/* Element scrolling: the embedder scrolls the whole page, not individual
 * boxes, so these stay 0 and the setter is a no-op (reading them must not
 * throw, which is what real pages rely on). */
static var_t* scroll_pos_get(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_int(vm, 0);
}
static var_t* scroll_pos_set(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return NULL;
}

static var_t* native_el_focus(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st != NULL && el != NULL && st->cb.el_focus != NULL) st->cb.el_focus(st->ctx, el);
    return NULL;
}

static var_t* native_el_scrollIntoView(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st != NULL && el != NULL && st->cb.el_scroll_into_view != NULL)
        st->cb.el_scroll_into_view(st->ctx, el);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* document.body / head / documentElement + the read-only URL family   */
/* ------------------------------------------------------------------ */

/* `data` is the bridge var as usual; which node to return is baked into the
 * three thin wrappers below (see doc_node()). */
static var_t* doc_node_get(vm_t* vm, void* data, int which) {
    js_dom_state* st = state_any(vm, data);
    return wrap_or_null(vm, doc_node(st, which));
}

static var_t* native_document_get_documentElement(vm_t* vm, var_t* env, void* data) {
    (void)env; return doc_node_get(vm, data, 0);
}
static var_t* native_document_get_head(vm_t* vm, var_t* env, void* data) {
    (void)env; return doc_node_get(vm, data, 1);
}
static var_t* native_document_get_body(vm_t* vm, var_t* env, void* data) {
    (void)env; return doc_node_get(vm, data, 2);
}

/* document.activeElement: the focused element, or <body> when nothing is. */
static var_t* native_document_get_activeElement(vm_t* vm, var_t* env, void* data) {
    (void)env;
    js_dom_state* st = state_any(vm, data);
    if(st != NULL && st->cb.get_active_element != NULL) {
        js_element_t el = st->cb.get_active_element(st->ctx);
        if(el != NULL) return wrap_or_null(vm, el);
    }
    return doc_node_get(vm, data, 2);
}

/* document.currentScript: the <script> element whose body is executing right
 * now, null outside a script run. Security SDKs (baxia) insert their loader
 * with currentScript.parentNode.insertBefore(...) and never install their
 * request signer when the property is missing. */
static var_t* native_document_get_currentScript(vm_t* vm, var_t* env, void* data) {
    (void)env;
    js_dom_state* st = state_any(vm, data);
    if(st != NULL && st->cb.get_current_script != NULL) {
        js_element_t el = st->cb.get_current_script(st->ctx);
        if(el != NULL) return wrap_or_null(vm, el);
    }
    return var_new_null(vm);
}

static var_t* native_document_get_url(vm_t* vm, var_t* env, void* data) {
    (void)env;
    js_dom_state* st = state_any(vm, data);
    if(st == NULL || st->cb.get_url == NULL) return var_new_str(vm, "");
    return adopt_cstr(vm, st->cb.get_url(st->ctx));
}

/* Constant, but pages probe them to decide whether they are in a browser. */
static var_t* native_document_get_readyState(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    /* Scripts only ever run once the document exists and is rendered. */
    return var_new_str(vm, "complete");
}

static var_t* native_document_get_empty_str(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_str(vm, "");
}

/* document.characterSet: the HTML parser always decodes as UTF-8 here. */
static var_t* native_document_get_charset(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_str(vm, "UTF-8");
}

/* Element.innerText is treated as an alias of textContent: this engine has no
 * layout-aware text extraction, and pages use the two interchangeably. */
static var_t* native_el_get_innerText(vm_t* vm, var_t* env, void* data) {
    return native_el_get_textContent(vm, env, data);
}
static var_t* native_el_set_innerText(vm_t* vm, var_t* env, void* data) {
    return native_el_set_textContent(vm, env, data);
}

/* ------------------------------------------------------------------ */
/* requestAnimationFrame                                              */
/*                                                                     */
/* A rAF callback is just a one-shot timer at the nominal frame period: */
/* the embedder already pumps js_dom_poll_timers() from its UI tick, so */
/* animations advance with the display without a second mechanism.      */
/* ------------------------------------------------------------------ */

/* 60fps. Pages that need more drive their own loop from the callback. */
#define JS_FRAME_MS 16

static var_t* native_requestAnimationFrame(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* cb = js_arg_func(env, 0);
    if(st == NULL || cb == NULL) return var_new_int(vm, 0);
    return var_new_int(vm, js_add_timer(vm, st, cb, JS_FRAME_MS, false));
}

static var_t* native_cancelAnimationFrame(vm_t* vm, var_t* env, void* data) {
    return native_clearTimeout(vm, env, data);
}

int64_t js_dom_monotonic_ms(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return (int64_t)time(NULL) * 1000;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

/* blur(): drop keyboard focus from the element when it currently holds it. */
static var_t* native_el_blur(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st != NULL && el != NULL && st->cb.el_blur != NULL) st->cb.el_blur(st->ctx, el);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Element: text-control selection (selectionStart/End, select(),      */
/* setSelectionRange()). Offsets are codepoints, as in the DOM.        */
/* ------------------------------------------------------------------ */

static var_t* sel_pos_get(vm_t* vm, var_t* env, void* data, int which) {
    (void)env;
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    int s = 0, e = 0;
    if(st != NULL && el != NULL && st->cb.el_get_sel != NULL &&
       st->cb.el_get_sel(st->ctx, el, &s, &e))
        return var_new_int(vm, which ? e : s);
    return var_new_int(vm, 0);   /* no editable selection here */
}
static var_t* native_el_get_selectionStart(vm_t* vm, var_t* env, void* data) {
    return sel_pos_get(vm, env, data, 0);
}
static var_t* native_el_get_selectionEnd(vm_t* vm, var_t* env, void* data) {
    return sel_pos_get(vm, env, data, 1);
}

/* Assigning one end keeps the range coherent: the other end collapses to it
 * when the new value crosses it (the caret-move behaviour browsers show). */
static var_t* sel_pos_set(vm_t* vm, var_t* env, void* data, int which) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_get_sel == NULL ||
       st->cb.el_set_sel == NULL)
        return NULL;
    int s = 0, e = 0;
    if(!st->cb.el_get_sel(st->ctx, el, &s, &e)) return NULL;
    int v = js_arg_int(env, 0);
    if(v < 0) v = 0;
    if(which) { e = v; if(s > e) s = e; }
    else      { s = v; if(e < s) e = s; }
    st->cb.el_set_sel(st->ctx, el, s, e);
    return NULL;
}
static var_t* native_el_set_selectionStart(vm_t* vm, var_t* env, void* data) {
    return sel_pos_set(vm, env, data, 0);
}
static var_t* native_el_set_selectionEnd(vm_t* vm, var_t* env, void* data) {
    return sel_pos_set(vm, env, data, 1);
}

static var_t* native_el_setSelectionRange(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_set_sel == NULL) return NULL;
    st->cb.el_set_sel(st->ctx, el, js_arg_int(env, 0), js_arg_int(env, 1));
    return NULL;
}

/* select(): the whole text. The embedder clamps, so an open-ended range is
 * safe to pass straight through. */
static var_t* native_el_select(vm_t* vm, var_t* env, void* data) {
    (void)env;
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_set_sel == NULL) return NULL;
    st->cb.el_set_sel(st->ctx, el, 0, 0x7FFFFFFF);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Element.dataset: a per-element DOMStringMap stand-in.               */
/*                                                                     */
/* The map object is cached on the bridge (keyed by the element handle */
/* so re-wrapped elements share it) and seeded once from the data-*    */
/* attributes; later script writes (card.dataset.id = ...) land on the */
/* cached object and stay visible to future reads.                    */
/* ------------------------------------------------------------------ */

static var_t* native_el_get_dataset(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL) return var_new_null(vm);
    var_t* bridge = (var_t*)data;
    var_t* bag = var_find_own_member_var(bridge, "@@datasets");
    if(bag == NULL) {
        bag = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(bridge, "@@datasets", bag);
    }
    char key[32];
    snprintf(key, sizeof(key), "%p", el);
    var_t* ds = var_find_own_member_var(bag, key);
    if(ds != NULL) return var_ref(ds);
    ds = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(bag, key, ds);
    if(st->cb.el_get_dataset != NULL) {
        char* raw = st->cb.el_get_dataset(st->ctx, el);
        if(raw != NULL) {
            char* p = raw;
            while(*p != 0) {
                char* sep = strchr(p, '\x1f');
                char* end = strchr(p, '\x1e');
                if(sep == NULL || end == NULL || sep > end) break;
                *sep = 0;
                *end = 0;
                var_add(ds, p, var_new_str(vm, sep + 1));
                p = end + 1;
            }
            mario_free(raw);
        }
    }
    return var_ref(ds);
}

/* Element.attributes: a snapshot NamedNodeMap - length plus indexed
 * {name, value} attribute nodes. w3.org's convertLinkToButton() copies every
 * non-href attribute of the top-level nav link onto the button it creates,
 * so without this the nav enhancement dies mid-loop. */
static var_t* native_el_get_attributes(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL) return var_new_null(vm);
    var_t* map = var_new_obj_no_proto(vm, NULL, NULL);
    vm->gc.gc_defer++;
    int n = 0;
    if(st->cb.el_attr_snapshot != NULL) {
        char* raw = st->cb.el_attr_snapshot(st->ctx, el);
        if(raw != NULL) {
            char* p = raw;
            while(*p != 0) {
                char* sep = strchr(p, '\x1f');
                char* end = strchr(p, '\x1e');
                if(sep == NULL || end == NULL || sep > end) break;
                *sep = 0;
                *end = 0;
                var_t* a = var_new_obj_no_proto(vm, NULL, NULL);
                var_add(a, "name", var_new_str(vm, p));
                var_add(a, "value", var_new_str(vm, sep + 1));
                char idx[16];
                snprintf(idx, sizeof(idx), "%d", n);
                var_add(map, idx, a);
                n++;
                p = end + 1;
            }
            mario_free(raw);
        }
    }
    var_add(map, "length", var_new_int(vm, n));
    vm->gc.gc_defer--;
    return map;
}

/* `new Image()` is the classic preloader idiom; per the constructor-override
 * rule (an object returned from a constructor replaces `this`), returning the
 * createElement('img') wrapper makes the result a fully usable element, so
 * `img.src = ...` / `img.onload = ...` behave exactly like a parsed <img>. */
static var_t* native_image_ctor(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    (void)env;
    if(st == NULL || st->cb.create_element == NULL) return NULL;
    js_element_t el = st->cb.create_element(st->ctx, "img");
    if(el == NULL) return NULL;
    return wrap_element(vm, el);
}

bool js_register_dom_natives(vm_t* vm, void* ctx, const js_dom_callbacks_t* cb) {
    if(vm == NULL || cb == NULL) return false;

    /* Allocate the bridge state and hang it off vm->root so that natives
     * invoked through method-call paths (where `data` may be the env) can
     * still recover it via state_from_vm(). */
    js_dom_state* st = (js_dom_state*)mario_malloc(sizeof(js_dom_state));
    if(st == NULL) return false;
    st->ctx = ctx;
    st->cb = *cb;
    st->write_buf = NULL;
    memset(st->timers, 0, sizeof(st->timers));
    st->timer_next_id  = 0;
    st->timer_last_now = 0;
    st->timer_synced   = false;
    st->el_cache       = NULL;
    st->el_cache_len   = 0;
    st->el_cache_cap   = 0;

    var_t* bridge = var_new_obj_no_proto(vm, st, dom_state_free);
    if(bridge == NULL) {
        mario_free(st);
        return false;
    }
    var_add(vm->root, DOM_BRIDGE_KEY, bridge);

    /* alert() is a global function. */
    vm_reg_static(vm, NULL, "alert(v)", native_alert, bridge);
    /* eval() runs global-scope (no lexical capture); see native_eval. */
    vm_reg_static(vm, NULL, "eval(src)", native_eval, bridge);

    /* Document class + singleton `document`. */
    var_t* doc_cls = vm_new_class(vm, CLS_DOCUMENT);
    vm_reg_native(vm, doc_cls, "write(v)", native_document_write, bridge);
    vm_reg_native(vm, doc_cls, "writeln(v)", native_document_writeln, bridge);
    vm_reg_native(vm, doc_cls, "getElementById(id)", native_document_getElementById, bridge);
    /* Selector queries. querySelectorAll/getElementsBy* return a plain JS
     * array, which already gives pages length/[i]/forEach/item-like use. */
    vm_reg_native(vm, doc_cls, "querySelector(sel)", native_document_querySelector, bridge);
    vm_reg_native(vm, doc_cls, "querySelectorAll(sel)", native_document_querySelectorAll, bridge);
    vm_reg_native(vm, doc_cls, "getElementsByTagName(tag)", native_document_getElementsByTagName, bridge);
    vm_reg_native(vm, doc_cls, "getElementsByClassName(cls)", native_document_getElementsByClassName, bridge);
    vm_reg_native(vm, doc_cls, "getElementsByName(name)", native_document_getElementsByName, bridge);
    vm_reg_native(vm, doc_cls, "createElement(tag)", native_document_createElement, bridge);
    vm_reg_native(vm, doc_cls, "createTextNode(text)", native_document_createTextNode, bridge);
    vm_reg_native(vm, doc_cls, "createComment(text)", native_document_createComment, bridge);
    vm_reg_native(vm, doc_cls, "createElementNS(ns,tag)", native_document_createElementNS, bridge);
    /* title / body as accessor properties so `document.title` and
     * `document.body.innerHTML` resolve like a real browser. */
    reg_accessor(vm, doc_cls, "title", native_document_get_title, native_document_set_title, bridge);
    reg_accessor(vm, doc_cls, "body", native_document_get_body, NULL, bridge);
    reg_accessor(vm, doc_cls, "activeElement", native_document_get_activeElement, NULL, bridge);
    reg_accessor(vm, doc_cls, "currentScript", native_document_get_currentScript, NULL, bridge);
    reg_accessor(vm, doc_cls, "scripts", native_document_get_scripts, NULL, bridge);
    reg_accessor(vm, doc_cls, "head", native_document_get_head, NULL, bridge);
    reg_accessor(vm, doc_cls, "documentElement", native_document_get_documentElement, NULL, bridge);
    reg_accessor(vm, doc_cls, "URL", native_document_get_url, NULL, bridge);
    reg_accessor(vm, doc_cls, "documentURI", native_document_get_url, NULL, bridge);
    reg_accessor(vm, doc_cls, "readyState", native_document_get_readyState, NULL, bridge);
    reg_accessor(vm, doc_cls, "domain", native_document_get_empty_str, NULL, bridge);
    reg_accessor(vm, doc_cls, "referrer", native_document_get_empty_str, NULL, bridge);
    reg_accessor(vm, doc_cls, "characterSet", native_document_get_charset, NULL, bridge);
    /* Node.nodeType for the Document singleton (9) plus the Node type
     * constants pages and frameworks read off document / Node. */
    {
        var_t* dproto = var_get_prototype(doc_cls);
        if(dproto != NULL) {
            var_add(dproto, "nodeType", var_new_int(vm, 9)); /* DOCUMENT_NODE */
            var_add(dproto, "ELEMENT_NODE", var_new_int(vm, 1));
            var_add(dproto, "TEXT_NODE", var_new_int(vm, 3));
            var_add(dproto, "COMMENT_NODE", var_new_int(vm, 8));
            var_add(dproto, "DOCUMENT_NODE", var_new_int(vm, 9));
            var_add(dproto, "DOCUMENT_FRAGMENT_NODE", var_new_int(vm, 11));
        }
    }

    var_t* document = new_obj(vm, CLS_DOCUMENT, 0);
    var_add(vm->root, "document", document);

    /* Element class. Instances are created on demand by getElementById /
     * body; there is no global Element binding to construct directly. */
    var_t* el_cls = vm_new_class(vm, CLS_ELEMENT);
    var_t* el_proto = var_get_prototype(el_cls);
    /* Web content references the standard DOM constructor globals -
     * `instanceof HTMLElement`, `class X extends HTMLElement`, feature tests on
     * Node/EventTarget. Only "Element" was registered, so those threw
     * "'HTMLElement' undefined!". Expose them as classes (bound on the global
     * scope by vm_new_class) so the references resolve and are extendable;
     * Element wrappers keep their own "Element" class. */
    var_t* htmlel_cls = vm_new_class(vm, "HTMLElement");
    var_t* node_cls   = vm_new_class(vm, "Node");
    vm_new_class(vm, "EventTarget");
    /* The same references appear for the other IDL bases web bundles test or
     * subclass (SVG markup, fragments, observers). vm_new_class reuses an
     * existing live binding, so these are no-ops if a real impl is registered
     * elsewhere (e.g. Event/CustomEvent in js_event.c). */
    vm_new_class(vm, "SVGElement");
    var_t* frag_cls   = vm_new_class(vm, "DocumentFragment");
    vm_new_class(vm, "MutationObserver");
    vm_new_class(vm, "IntersectionObserver");
    vm_new_class(vm, "ResizeObserver");
    reg_accessor(vm, el_cls, "textContent", native_el_get_textContent, native_el_set_textContent, bridge);
    reg_accessor(vm, el_cls, "nodeValue", native_el_get_nodeValue, native_el_set_nodeValue, bridge);
    reg_accessor(vm, el_cls, "data", native_el_get_nodeValue, native_el_set_nodeValue, bridge);
    reg_accessor(vm, el_cls, "innerText", native_el_get_innerText, native_el_set_innerText, bridge);
    reg_accessor(vm, el_cls, "innerHTML", native_el_get_innerHTML, native_el_set_innerHTML, bridge);
    reg_accessor(vm, el_cls, "tagName", native_el_get_tagName, NULL, bridge);
    reg_accessor(vm, el_cls, "nodeName", native_el_get_tagName, NULL, bridge);
    reg_accessor(vm, el_cls, "nodeType", native_el_get_nodeType, NULL, bridge);
    reg_accessor(vm, el_cls, "className", native_el_get_className, native_el_set_className, bridge);
    reg_accessor(vm, el_cls, "classList", native_el_get_classList, NULL, bridge);
    reg_accessor(vm, el_cls, "style", native_el_get_style, NULL, bridge);
    vm_reg_native(vm, el_cls, "getAttribute(name)", native_el_getAttribute, bridge);
    vm_reg_native(vm, el_cls, "setAttribute(name, value)", native_el_setAttribute, bridge);
    vm_reg_native(vm, el_cls, "hasAttribute(name)", native_el_hasAttribute, bridge);
    vm_reg_native(vm, el_cls, "removeAttribute(name)", native_el_removeAttribute, bridge);

    /* Attribute-backed and boolean properties: one accessor pair per table
     * row, with the row index as the native's `data`. */
    for(int i = 0; i < ATTR_PROP_COUNT; ++i)
        js_acc_cls(vm, el_cls, kAttrProps[i].js, attr_prop_get, attr_prop_set,
                   (void*)(intptr_t)i);
    for(int i = 0; i < BOOL_PROP_COUNT; ++i)
        js_acc_cls(vm, el_cls, kBoolProps[i].js, bool_prop_get, bool_prop_set,
                   (void*)(intptr_t)i);

    /* Tree walking. */
    reg_accessor(vm, el_cls, "children", native_el_get_children, NULL, bridge);
    reg_accessor(vm, el_cls, "childNodes", native_el_get_childNodes, NULL, bridge);
    reg_accessor(vm, el_cls, "childElementCount", native_el_get_childElementCount, NULL, bridge);
    reg_accessor(vm, el_cls, "parentNode", native_el_get_parentNode, NULL, bridge);
    reg_accessor(vm, el_cls, "parentElement", native_el_get_parentNode, NULL, bridge);
    reg_accessor(vm, el_cls, "ownerDocument", native_el_get_ownerDocument, NULL, bridge);
    reg_accessor(vm, el_cls, "namespaceURI", native_el_get_namespaceURI, NULL, bridge);
    reg_accessor(vm, el_cls, "firstChild", native_el_get_firstChild, NULL, bridge);
    reg_accessor(vm, el_cls, "lastChild", native_el_get_lastChild, NULL, bridge);
    reg_accessor(vm, el_cls, "firstElementChild", native_el_get_firstElementChild, NULL, bridge);
    reg_accessor(vm, el_cls, "lastElementChild", native_el_get_lastElementChild, NULL, bridge);
    reg_accessor(vm, el_cls, "nextSibling", native_el_get_nextSibling, NULL, bridge);
    reg_accessor(vm, el_cls, "previousSibling", native_el_get_previousSibling, NULL, bridge);
    reg_accessor(vm, el_cls, "nextElementSibling", native_el_get_nextElementSibling, NULL, bridge);
    reg_accessor(vm, el_cls, "previousElementSibling", native_el_get_previousElementSibling, NULL, bridge);
    reg_accessor(vm, el_cls, "index", native_el_get_index, NULL, bridge);

    /* Mutation. */
    vm_reg_native(vm, el_cls, "appendChild(node)", native_el_appendChild, bridge);
    vm_reg_native(vm, el_cls, "append(node)", native_el_append, bridge);
    vm_reg_native(vm, el_cls, "insertBefore(node, ref)", native_el_insertBefore, bridge);
    vm_reg_native(vm, el_cls, "removeChild(node)", native_el_removeChild, bridge);
    vm_reg_native(vm, el_cls, "replaceChild(node, old)", native_el_replaceChild, bridge);
    vm_reg_native(vm, el_cls, "cloneNode(deep)", native_el_cloneNode, bridge);
    vm_reg_native(vm, el_cls, "remove()", native_el_remove, bridge);
    vm_reg_native(vm, el_cls, "contains(node)", native_el_contains, bridge);

    /* Real DOM hangs the mutation + tree-walking surface off Node.prototype, so
     * EVERY node kind (Document, DocumentFragment, Element, ...) inherits it.
     * mario classes are flat (no shared Node base), so mirror the surface onto
     * the other node classes: React-DOM and the security SDKs call
     * insertBefore/appendChild/replaceChild on fragments and on document, which
     * otherwise miss ("can not find function 'insertBefore'") and abort the
     * mount. Element wrappers keep el_cls; these classes serve subclasses and
     * any wrapper bound to them. */
    {
        /* doc_cls too: Document IS a Node in the real DOM, and `with(document)`
         * + a bare `insertBefore(...)` (taobao's beacon bootstrap) resolves the
         * method on the document wrapper when the inner with-object misses. The
         * document singleton carries no element handle, so the natives degrade
         * to null/no-op for it - name RESOLUTION is what matters here. */
        var_t* node_classes[4] = { node_cls, frag_cls, htmlel_cls, doc_cls };
        for(int ci = 0; ci < 4; ++ci) {
            var_t* nc = node_classes[ci];
            if(nc == NULL || nc == el_cls) continue;
            reg_accessor(vm, nc, "parentNode", native_el_get_parentNode, NULL, bridge);
            reg_accessor(vm, nc, "parentElement", native_el_get_parentNode, NULL, bridge);
            reg_accessor(vm, nc, "ownerDocument", native_el_get_ownerDocument, NULL, bridge);
            reg_accessor(vm, nc, "namespaceURI", native_el_get_namespaceURI, NULL, bridge);
            reg_accessor(vm, nc, "childNodes", native_el_get_childNodes, NULL, bridge);
            reg_accessor(vm, nc, "children", native_el_get_children, NULL, bridge);
            reg_accessor(vm, nc, "firstChild", native_el_get_firstChild, NULL, bridge);
            reg_accessor(vm, nc, "lastChild", native_el_get_lastChild, NULL, bridge);
            vm_reg_native(vm, nc, "appendChild(node)", native_el_appendChild, bridge);
            vm_reg_native(vm, nc, "append(node)", native_el_append, bridge);
            vm_reg_native(vm, nc, "insertBefore(node, ref)", native_el_insertBefore, bridge);
            vm_reg_native(vm, nc, "removeChild(node)", native_el_removeChild, bridge);
            vm_reg_native(vm, nc, "replaceChild(node, old)", native_el_replaceChild, bridge);
            vm_reg_native(vm, nc, "cloneNode(deep)", native_el_cloneNode, bridge);
            vm_reg_native(vm, nc, "remove()", native_el_remove, bridge);
            vm_reg_native(vm, nc, "contains(node)", native_el_contains, bridge);
        }
    }

    /* Scoped queries. */
    vm_reg_native(vm, el_cls, "querySelector(sel)", native_el_querySelector, bridge);
    vm_reg_native(vm, el_cls, "querySelectorAll(sel)", native_el_querySelectorAll, bridge);
    vm_reg_native(vm, el_cls, "getElementsByTagName(tag)", native_el_getElementsByTagName, bridge);
    vm_reg_native(vm, el_cls, "getElementsByClassName(cls)", native_el_getElementsByClassName, bridge);
    vm_reg_native(vm, el_cls, "matches(sel)", native_el_matches, bridge);
    vm_reg_native(vm, el_cls, "closest(sel)", native_el_closest, bridge);

    /* Geometry + focus. */
    vm_reg_native(vm, el_cls, "getBoundingClientRect()", native_el_getBoundingClientRect, bridge);
    vm_reg_native(vm, el_cls, "focus()", native_el_focus, bridge);
    vm_reg_native(vm, el_cls, "blur()", native_el_blur, bridge);
    /* Text-control selection surface. */
    reg_accessor(vm, el_cls, "selectionStart", native_el_get_selectionStart, native_el_set_selectionStart, bridge);
    reg_accessor(vm, el_cls, "selectionEnd",   native_el_get_selectionEnd,   native_el_set_selectionEnd,   bridge);
    vm_reg_native(vm, el_cls, "select()", native_el_select, bridge);
    vm_reg_native(vm, el_cls, "setSelectionRange(a,b)", native_el_setSelectionRange, bridge);
    reg_accessor(vm, el_cls, "dataset", native_el_get_dataset, NULL, bridge);
    reg_accessor(vm, el_cls, "attributes", native_el_get_attributes, NULL, bridge);
    vm_reg_native(vm, el_cls, "scrollIntoView(a)", native_el_scrollIntoView, bridge);
    static const char* kGeomProps[] = {
        "offsetWidth", "offsetHeight", "offsetLeft", "offsetTop",
        "clientWidth", "clientHeight", "scrollWidth", "scrollHeight"
    };
    for(int i = 0; i < (int)(sizeof(kGeomProps) / sizeof(kGeomProps[0])); ++i)
        js_acc_on(vm, el_proto, kGeomProps[i], geom_get, NULL, (void*)(intptr_t)i);
    js_acc_on(vm, el_proto, "scrollTop", scroll_pos_get, scroll_pos_set, bridge);
    js_acc_on(vm, el_proto, "scrollLeft", scroll_pos_get, scroll_pos_set, bridge);

    /* CSSStyleDeclaration: element.style. The camelCase accessors share two
     * natives dispatched by their index into kCssProps. */
    var_t* style_cls = vm_new_class(vm, CLS_STYLE);
    var_t* style_proto = var_get_prototype(style_cls);
    js_acc_cls(vm, style_cls, "cssText", style_get_cssText, style_set_cssText, bridge);
    js_acc_cls(vm, style_cls, "length", style_get_length, NULL, bridge);
    vm_reg_native(vm, style_cls, "getPropertyValue(p)", style_getPropertyValue, bridge);
    vm_reg_native(vm, style_cls, "setProperty(p, v)", style_setProperty, bridge);
    vm_reg_native(vm, style_cls, "removeProperty(p)", style_removeProperty, bridge);
    vm_reg_native(vm, style_cls, "item(i)", style_item, bridge);
    for(int i = 0; i < CSS_PROP_COUNT; ++i)
        js_acc_on(vm, style_proto, kCssProps[i].js, style_prop_get, style_prop_set,
                  (void*)(intptr_t)i);

    /* DOMTokenList: element.classList. */
    var_t* tok_cls = vm_new_class(vm, CLS_TOKENS);
    vm_reg_native(vm, tok_cls, "add(t)", native_classList_add, bridge);
    vm_reg_native(vm, tok_cls, "remove(t)", native_classList_remove, bridge);
    vm_reg_native(vm, tok_cls, "toggle(t, force)", native_classList_toggle, bridge);
    vm_reg_native(vm, tok_cls, "contains(t)", native_classList_contains, bridge);
    vm_reg_native(vm, tok_cls, "replace(a, b)", native_classList_replace, bridge);
    vm_reg_native(vm, tok_cls, "item(i)", native_classList_item, bridge);
    vm_reg_native(vm, tok_cls, "toString()", native_classList_toString, bridge);
    js_acc_cls(vm, tok_cls, "length", native_classList_get_length, NULL, bridge);
    js_acc_cls(vm, tok_cls, "value", native_classList_get_value, native_classList_set_value, bridge);

    /* window.location.href. `window` is the global object itself (see below);
     * `location` is an instance of Location with an href getter (js_web.c
     * fills in the rest of the Location surface: protocol/host/pathname/
     * search/hash/assign/...). */
    var_t* loc_cls = vm_new_class(vm, CLS_LOCATION);
    reg_accessor(vm, loc_cls, "href", native_location_get_href, NULL, bridge);

    /* `new Image()` constructs an <img> element wrapper (preloader idiom). */
    var_t* img_cls = vm_new_class(vm, "Image");
    vm_reg_native(vm, img_cls, "constructor(w, h)", native_image_ctor, bridge);

    var_t* location = new_obj(vm, CLS_LOCATION, 0);
    /* A browser's `window` IS the global object: a UMD bundle that exports
     * via `window.FontFaceObserver = ...` (w3.org's fontfaceobserver.js) has
     * to be reachable as a bare global, and `window.foo = x` must be visible
     * to every later script. mario keeps the global scope in vm->root, so
     * alias that instead of hanging a plain object off it; the self member
     * gives the browser-identical window.window === window. web_publish()
     * already skips the mirror step when window == vm->root. */
    var_t* window = vm->root;
    var_add(window, "location", location);
    var_add(vm->root, "window", window);
    /* window.document / document.defaultView: both directions of the pair
     * pages use to hop between the global and the document. */
    var_add(window, "document", document);
    var_add(document, "defaultView", window);

    /* Timers are global functions, registered on vm->root exactly like alert()
     * above, with `bridge` as the native `data` so state_any() recovers st. */
    vm_reg_static(vm, NULL, "setInterval(fn, ms)", native_setInterval,   bridge);
    vm_reg_static(vm, NULL, "setTimeout(fn, ms)",  native_setTimeout,    bridge);
    vm_reg_static(vm, NULL, "clearInterval(id)",   native_clearInterval, bridge);
    vm_reg_static(vm, NULL, "clearTimeout(id)",    native_clearTimeout,  bridge);
    vm_reg_static(vm, NULL, "requestAnimationFrame(fn)",  native_requestAnimationFrame,  bridge);
    vm_reg_static(vm, NULL, "cancelAnimationFrame(id)",   native_cancelAnimationFrame,   bridge);

    /* Override the engine's Math (radians sin/cos, numeric PI/E) and Date
     * (int64 stamp + wall-clock getters) on their existing prototypes. */
    js_patch_math(vm);
    js_patch_date(vm);

    return true;
}

void js_dom_reset_element_cache(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    if(st == NULL) return;
    /* Clear the handle->wrapper table, then rebuild the @@elcache anchor as an
     * empty array. var_add replaces the old anchor and unrefs it, which drops
     * the cache's one reference to each wrapper; anything a script still holds
     * stays alive (refs > 0) and anything it dropped is collected. el_free is a
     * no-op, so no handle is ever freed here - they belong to the embedder. */
    for(int i = 0; i < st->el_cache_len; ++i) {
        st->el_cache[i].handle = NULL;
        st->el_cache[i].obj    = NULL;
    }
    st->el_cache_len = 0;
    js_reanchor_el_cache(vm, st);
}

/* ------------------------------------------------------------------ */
/* Accessors for the sibling bridges (js_event.c / js_web.c)           */
/*                                                                     */
/* They share this bridge's embedder context, callback table, Element  */
/* wrappers and timer table instead of making the embedder register    */
/* the same pointers twice.                                            */
/* ------------------------------------------------------------------ */

void* js_dom_ctx(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    return (st != NULL) ? st->ctx : NULL;
}

const js_dom_callbacks_t* js_dom_callbacks(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    return (st != NULL) ? &st->cb : NULL;
}

var_t* js_dom_wrap_element(vm_t* vm, js_element_t el) {
    return wrap_or_null(vm, el);
}

var_t* js_dom_wrap_style(vm_t* vm, js_element_t el) {
    return (el == NULL) ? var_new_null(vm) : wrap_style(vm, el);
}

var_t* js_dom_element_class(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return NULL;
    return var_find_own_member_var(vm->root, CLS_ELEMENT);
}

int js_dom_add_timer(vm_t* vm, var_t* cb, uint32_t ms, bool repeat) {
    js_dom_state* st = state_from_vm(vm);
    if(st == NULL || cb == NULL || !cb->is_func) return 0;
    return js_add_timer(vm, st, cb, ms, repeat);
}

void js_dom_clear_timer(vm_t* vm, int id) {
    js_dom_state* st = state_from_vm(vm);
    if(st == NULL || id <= 0) return;
    js_clear_timer(vm, st, id);
}

char* js_dom_take_write_buffer(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    if(st == NULL || st->write_buf == NULL || st->write_buf->len == 0) {
        /* Contract: never NULL. Return an empty mario_malloc'd string so the
         * caller can unconditionally mario_free() the result. */
        char* empty = (char*)mario_malloc(1);
        if(empty != NULL) empty[0] = 0;
        return empty;
    }
    /* Detach the buffer so subsequent writes start fresh, then hand the
     * caller a plain mario_malloc'd copy it can free with mario_free(). */
    uint32_t len = st->write_buf->len;
    char* out = (char*)mario_malloc(len + 1);
    if(out == NULL) return NULL;
    memcpy(out, st->write_buf->cstr, len);
    out[len] = 0;
    mstr_reset(st->write_buf);
    return out;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */
