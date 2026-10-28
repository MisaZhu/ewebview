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
#define CLS_FRAGMENT "DocumentFragment"
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
 * path allocation-free. It must be LARGE: queueMicrotask()/MessageChannel/
 * MutationObserver all ride this table at 0 ms, and a single synchronous
 * script burst (core-js + React flight on rokid.com) arms hundreds of them
 * before the loop gets a chance to poll and drain - with a small cap the
 * overflow adds return 0 and the callbacks are silently DROPPED, which
 * strands the flight read-loop and stalls the whole page. */
#define JS_TIMER_MAX 2048

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
    /* A microtask (promise reaction, queueMicrotask, process.nextTick) is due
     * IMMEDIATELY (remaining_ms 0) and the poll loop drains every pending one
     * before - and again after - each macrotask (setTimeout/MessageChannel).
     * Without this priority the engine fired 0-ms macrotasks ahead of promise
     * reactions, inverting the spec's microtask-before-macrotask ordering and
     * deadlocking React 18's concurrent scheduler (its MessageChannel work
     * loop ran before the flight ping microtask, re-suspended, then reused a
     * spent task forever). */
    bool                  microtask;
    /* requestIdleCallback: the callback receives an IdleDeadline object
     * ({didTimeout, timeRemaining()}) as its single argument, which the plain
     * timer path would never supply. The engine has no real idle notion, so the
     * deadline always reports zero remaining time. */
    bool                  idle;
} js_timer_t;

typedef struct {
    void*                 ctx;
    js_dom_callbacks_t    cb;
    mstr_t*               write_buf;   /* document.write accumulator */
    js_timer_t            timers[JS_TIMER_MAX];
    int                   timer_next_id;  /* monotonic id source */
    long                  timer_drops;    /* callbacks lost to table overflow */
    uint64_t              timer_last_now; /* embedder clock at the previous poll */
    bool                  timer_synced;   /* false until the first poll seeds it */
    js_el_cache_t*        el_cache;       /* growable, never evicted (see above) */
    int                   el_cache_len;
    int                   el_cache_cap;
    bool                  dom_loaded;     /* false until DOMContentLoaded fires */
    bool                  parsing_complete; /* true while deferred/module scripts run */
    /* True while an ASYNC event-loop callback runs (timer / MessageChannel /
     * promise reaction / event handler - jsVmEnter(appScript=false)). A real
     * browser only ever runs those AFTER the parser finished, so they observe
     * readyState=="interactive", never "loading". This engine parses the whole
     * document before running any script, yet reports "loading" during the SYNC
     * script phase so Next.js's inline flight bootstrap defers its stream-close
     * to DOMContentLoaded (see native_document_get_readyState). pinterest.com's
     * app scripts are async CLASSIC webpack chunks (no type=module), so nothing
     * ever flips parsing_complete and its MessageChannel-scheduled React render
     * saw "loading": the dehydrated-Suspense client-render gate
     * (c4: "$?"===data && readyState!=="loading") then deferred the boundary to
     * the idle lane and the CSR shell never filled. This flag gives the async
     * callbacks the post-parse value while sync bodies keep "loading". */
    bool                  in_async_cb;
    /* While true, customElements.define() records the registration but queues
     * its bulk connectedCallback upgrade instead of running it inline (see
     * js_dom_set_defer_upgrades). github.com's react-app connectedCallback
     * awaits a code-view registration that only a LATER document-order script
     * performs; upgrading inline (mid script_4) forced the synchronous await
     * spin / re-entrant script pump that corrupts the suspended webpack
     * factory frames. Deferring the reaction to after the post-swap script
     * queue lets the registration exist first, so the await settles normally. */
    bool                  defer_upgrades;
    /* Custom-element insertion reactions. customElements.define() runs a
     * one-shot bulk upgrade over the elements that exist AT DEFINE TIME. But
     * github's React fill commit (post deferred-data) appends <relative-time>
     * and other catalyst elements LONG AFTER their define() ran with
     * matched=0, so those instances never get a connectedCallback and render
     * as permanent grey skeletons (relative-time never formats its datetime).
     * A real browser fires an upgrade reaction on insertion; we approximate it
     * by recording the tag name of every registered custom element that gets
     * inserted (see ce_mark_dirty in mo_notify_childlist), then re-running the
     * idempotent ce_upgrade_all for just those names once the JS that inserted
     * them settles (js_dom_upgrade_dirty, called from the engine tick after
     * timers/fetch fire). Bounded: only names actually inserted are re-scanned,
     * and ce_upgrade_all skips elements already carrying @@ceupg. */
#define CE_DIRTY_MAX  48
#define CE_DIRTY_NAME 48
    char                  ce_dirty[CE_DIRTY_MAX][CE_DIRTY_NAME];
    int                   ce_dirty_n;
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
    /* Tag nodes are reparented onto HTMLElement.prototype so the real DOM
     * prototype chain holds: HTMLElement.prototype inherits Element.prototype
     * (linked in js_register_dom_natives), which inherits Node/EventTarget. A
     * <div> is therefore `instanceof HTMLElement`, `instanceof Element` AND
     * `instanceof Node`, exactly as in a browser, and the native Element methods
     * still resolve up the chain. Wrapping every element as plain "Element" made
     * `el instanceof HTMLElement` false, breaking the guard sites React and
     * countless libraries use (e.g. Pinterest's shell-loader teardown
     * `e instanceof HTMLElement && body.removeChild(e)`). Done by reparenting
     * rather than new_obj("HTMLElement") so the upgrade-aware HTMLElement @@ctor
     * (native_htmlel_ctor / s_ce_pending_el) is never triggered from here.
     * Non-tag nodes (text/comment) keep the bare Element wrapper as before. */
    if(st != NULL && st->cb.el_is_tag != NULL && st->cb.el_is_tag(st->ctx, el)) {
        var_t* hcls = var_find_own_member_var(vm->root, "HTMLElement");
        if(hcls != NULL) {
            var_t* hproto = var_get_prototype(hcls);
            if(hproto != NULL) var_set_prototype(obj, hproto);
        }
    }
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

/* Forward decls: the custom-element insertion-reaction helpers live further
 * down (with the MutationObserver hooks) but innerHTML assignment - which
 * bypasses mo_notify_childlist - also needs them. */
static void ce_mark_dirty(vm_t* vm, js_dom_state* st, js_element_t added);
static void ce_mark_subtree_dirty(vm_t* vm, js_dom_state* st, js_element_t el, int depth);

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
        /* The port parses and attaches the fragment internally, so custom
         * elements created by innerHTML never pass through mo_notify_childlist
         * and would miss their insertion upgrade reaction. Walk the freshly
         * set subtree and mark any registered custom element for the drain.
         * Skipped entirely when no custom element is defined yet. */
        if(var_find_member_var(vm->root, "@@customelements") != NULL)
            ce_mark_subtree_dirty(vm, st, el, 0);
    }
    return NULL;
}

/* Element.id is served by the attribute-property table below (kAttrProps),
 * which also gives it a setter. */

/* Env-gated (EWEB_HYDDBG) DOM-walk trace: prints each firstChild/nextSibling/
 * tagName touch so a hydration desync can be located by replaying the walk
 * against the SSR markup. el_get_tag returns an owned string; free after use. */
static void hydw_log(const char* op, js_dom_state* st, js_element_t el) {
    static int on = -1;
    if(on < 0) on = (getenv("EWEB_HYDDBG") != NULL);
    if(!on) return;
    if(st == NULL || el == NULL) { fprintf(stderr, "[HYDW] %s <null>\n", op); return; }
    if(st->cb.el_is_tag != NULL && !st->cb.el_is_tag(st->ctx, el)) {
        fprintf(stderr, "[HYDW] %s #text\n", op); return;
    }
    if(st->cb.el_get_tag == NULL) { fprintf(stderr, "[HYDW] %s <?>\n", op); return; }
    char* tag = st->cb.el_get_tag(st->ctx, el);
    fprintf(stderr, "[HYDW] %s <%s>\n", op, tag ? tag : "?");
    free(tag);
}

static var_t* native_el_get_tagName(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    hydw_log("tn", st, el);
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

/* Element.localName is the lower-case tag name for HTML elements. Virtual DOM
 * hydrators (including the one used by Apple's global header) use localName to
 * match server-rendered nodes. Returning undefined makes every element look
 * mismatched, so a second client tree is inserted beside the SSR tree. */
static var_t* native_el_get_localName(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    if(st == NULL || el == NULL || st->cb.el_get_tag == NULL)
        return var_new_null(vm);
    if(st->cb.el_is_tag != NULL && !st->cb.el_is_tag(st->ctx, el))
        return var_new_null(vm);
    return adopt_cstr(vm, st->cb.el_get_tag(st->ctx, el));
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
    int t;
    if(st->cb.el_is_comment != NULL && st->cb.el_is_comment(st->ctx, el))
        t = 8; /* COMMENT_NODE */
    else if(st->cb.el_is_tag != NULL && !st->cb.el_is_tag(st->ctx, el))
        t = 3; /* TEXT_NODE */
    else
        t = 1; /* ELEMENT_NODE */
    {   /* HYDDBG: nodeType reads with the node identity */
        static int on = -1;
        if(on < 0) on = (getenv("EWEB_HYDDBG") != NULL);
        if(on) {
            char* g = (st->cb.el_get_tag != NULL) ? st->cb.el_get_tag(st->ctx, el) : NULL;
            fprintf(stderr, "[HYD] nt <%s> = %d\n", g ? (g[0] ? g : "#text") : "?", t);
            free(g);
        }
    }
    return var_new_int(vm, t);
}

/* MutationObserver delivery hook (defined with the observer natives below):
 * after a characterData (.data/.nodeValue) or attribute write, asynchronously
 * fire any observer watching this node. is_cd: 1 = characterData, 0 = attributes. */
static void mo_notify_charattr(vm_t* vm, js_element_t el, int is_cd, const char* attr);

/* MutationObserver delivery hook for tree mutations: after appendChild/append/
 * prepend/insertBefore/removeChild/replaceChild, fire observers whose target is
 * `parent` or - when they passed subtree:true - any ancestor of it. added /
 * removed are the inserted / removed node handles (may be NULL); the wrappers
 * for the record are built lazily, only when an observer is registered. */
static void mo_notify_childlist(vm_t* vm, js_dom_state* st, js_element_t parent,
                                js_element_t added, js_element_t removed);

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
    {
        const char* txt = st->cb.el_get_text(st->ctx, el);
        static int hyd_dbg = -1;
        if(hyd_dbg < 0) hyd_dbg = (getenv("EWEB_HYDDBG") != NULL);
        if(hyd_dbg) {
            static int ctr = 0;
            fprintf(stderr, "[HYD] nv#%d <%s>\n", ctr++, txt ? txt : "");
        }
        return adopt_cstr(vm, txt);
    }
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
    if(st->cb.el_set_text != NULL) {
        st->cb.el_set_text(st->ctx, el, s ? s : "");
    }
    mo_notify_charattr(vm, el, 1, NULL);   /* MutationObserver: characterData */
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
    char* v = st->cb.el_get_attr(st->ctx, el, name);
    {   /* HYDDBG: attribute reads - React's hydration diff probes props here */
        static int on = -1;
        if(on < 0) on = (getenv("EWEB_HYDDBG") != NULL);
        if(on) {
            char* t = (st->cb.el_get_tag != NULL) ? st->cb.el_get_tag(st->ctx, el) : NULL;
            fprintf(stderr, "[HYD] ga <%s> %s=%s\n", t ? (t[0] ? t : "#text") : "?",
                    name, v ? v : "(null)");
            free(t);
        }
    }
    return adopt_cstr(vm, v);
}

static var_t* native_el_setAttribute(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = handle_from_this(this_from_env(env));
    var_t* args = get_func_args(env);
    node_t* nn = var_array_get(args, 0);
    node_t* vn = var_array_get(args, 1);
    const char* name = (nn != NULL) ? var_get_str(nn->var) : NULL;
    const char* value = (vn != NULL) ? var_get_str(vn->var) : "";
    /* EWEB_IMGDBG: trace whether the carousel cover-image <img> is ever
     * created by React (setAttribute data-test-id / a pinimg src). */
    {
        static int s_coverdbg = -1;
        if(s_coverdbg < 0) { const char* e = getenv("EWEB_IMGDBG"); s_coverdbg = (e && e[0] == '1') ? 1 : 0; }
        if(s_coverdbg && name != NULL && value != NULL) {
            if((strcmp(name, "data-test-id") == 0 && strstr(value, "cover") != NULL) ||
               (strcmp(name, "src") == 0 && (strstr(value, "1200x") != NULL || strstr(value, "control1") != NULL)))
                fprintf(stderr, "[IMGDBG] coverimg setAttr %s=%.80s\n", name, value);
        }
    }
    if(st != NULL && el != NULL && name != NULL && st->cb.el_set_attr != NULL) {
        st->cb.el_set_attr(st->ctx, el, name, value ? value : "");
        mo_notify_charattr(vm, el, 0, name);   /* MutationObserver: attributes */
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
    /* @@t is the fresher store: mario's native Date setters (setDate/setMonth/
     * setFullYear/setHours/... - none of which js_patch_date overrides) write ONLY
     * @@t through date_store_ms, while js_dom's own ctor/setTime mirror both
     * stores. Reading @@t first keeps every getter (getTime/valueOf/getFullYear/
     * ...) consistent no matter which setter family ran last; before this, a
     * native-setter mutation left ->value stale and the js_dom getters read the
     * pre-mutation stamp (github relative-time's roundToSingleUnit month/ year
     * correction collapsed to 0 that way, formatting 3-day-old dates as
     * "in 0 seconds" and 8-year-old ones as "2 weeks ago"). */
    var_t* t = var_find_member_var(this_v, "@@t");
    if(t != NULL && t->type == V_INT64)
        return var_get_int64(t);
    return (int64_t)(intptr_t)this_v->value;
}

/* Parse an ISO-8601 / RFC-3339 date string ("2026-09-05T19:14:39.000+08:00",
 * "2026-09-05", "...Z") into epoch ms. github's committedDate/createdAt arrive as
 * strings, and var_get_float() on a string yields 0 (epoch), which rendered every
 * row as "Thu Jan 01 1970". Date-only forms are UTC midnight per spec; a trailing
 * Z forces UTC; an explicit +/-HH:MM shifts; otherwise the fields are local time.
 * Returns false when the text is not a recognisable date. */
static bool js_date_parse_iso(const char* s, int64_t* out) {
    if(s == NULL || s[0] == 0 || out == NULL) return false;
    int y = 1970, mo = 1, d = 1, h = 0, mi = 0, se = 0, fms = 0;
    if(sscanf(s, "%d-%d-%d", &y, &mo, &d) < 1) return false;
    const char* t = strchr(s, 'T');
    if(t == NULL) t = strchr(s, ' ');
    bool has_time = (t != NULL);
    bool utc = false;
    int tz_sign = 0, tz_h = 0, tz_m = 0;
    if(has_time) {
        t++;
        sscanf(t, "%d:%d:%d", &h, &mi, &se);
        const char* dot = strchr(t, '.');
        if(dot != NULL) fms = (int)(atof(dot) * 1000.0 + 0.5);
        if(strchr(t, 'Z') != NULL || strchr(t, 'z') != NULL) utc = true;
        else {
            const char* sg = NULL;
            for(const char* q = t + strlen(t) - 1; q > t; q--) {
                if(*q == '+' || *q == '-') { sg = q; break; }
            }
            if(sg != NULL) {
                tz_sign = (*sg == '+') ? 1 : -1;
                sscanf(sg + 1, "%d:%d", &tz_h, &tz_m);
            }
        }
    }
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = se;
    int64_t ms;
    if(has_time && !utc && tz_sign == 0) {
        ms = (int64_t)mktime(&tmv) * 1000 + fms;      /* local wall time */
    } else {
        ms = (int64_t)timegm(&tmv) * 1000 + fms;      /* UTC (date-only or Z) */
        if(tz_sign != 0)
            ms -= (int64_t)tz_sign * ((int64_t)tz_h * 3600 + tz_m * 60) * 1000;
    }
    *out = ms;
    return true;
}

/* js_patch_date overrides Date's constructor/getTime/getHours/... but NOT
 * toString/toISOString/toLocaleDateString/toJSON/toDateString/toTimeString:
 * those still resolve to mario's native_Date.c implementations, which read the
 * time from a hidden "@@t" member (not from obj->value). Without mirroring the
 * stamp there, every formatted date renders "Invalid Date". Keep the two stores
 * in sync: obj->value is authoritative for js_dom's own methods, "@@t" feeds
 * mario's un-overridden ones. */
static void js_date_stamp_mario(vm_t* vm, var_t* obj, int64_t ms) {
    if(vm == NULL || obj == NULL || obj->type != V_OBJECT) return;
    node_t* sn = var_find_own_member(obj, "@@t");
    if(sn != NULL && sn->var != NULL) {
        var_set_int64(sn->var, ms);
    } else {
        sn = var_add(obj, "@@t", var_new_int64(vm, ms));
        if(sn != NULL) { sn->be_unenumerable = 1; sn->invisable = 1; }
    }
}

/* Mirrors native_Date.c's DATE_INVALID sentinel (that header is not visible
 * here); every js_dom Date entry point must map it to NaN / "Invalid Date"
 * exactly like the native side does. */
#define JS_DATE_INVALID  INT64_MIN

/* ToNumber result guarded against NaN: the C cast (int64_t)NAN is undefined
 * behaviour (and differs between arm64/x86), so route NaN to the invalid
 * sentinel instead. */
static int64_t js_date_to_ms(vm_t* vm, var_t* arg) {
    double dv = vm_to_number(vm, arg);
    if(isnan(dv)) return JS_DATE_INVALID;
    return (int64_t)dv;
}

static var_t* js_date_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* obj = this_from_env(env);
    if(obj == NULL) return NULL;
    uint32_t argc = get_func_args_num(env);
    int64_t ms;
    if(argc >= 2) {
        /* new Date(y, m[, d, h, mi, s, ms]): composed in LOCAL time per spec.
         * Before this the ctor only honoured a single `value` argument, so the
         * multi-arg form fell into the numeric path and stamped the YEAR as the
         * epoch-ms (getTime()===2026), making getFullYear()/Intl/toLocale* all
         * read 1970 - the text mismatch behind rokid.com's React #418. */
        int64_t v[7];
        for(int i = 0; i < 7; i++)
            v[i] = (i < (int)argc) ? (int64_t)vm_to_number(vm, get_func_arg(env, i)) : 0;
        int64_t y = v[0];
        if(y >= 0 && y <= 99) y += 1900;   /* JS two-digit year mapping */
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = (int)(y - 1900);
        tm.tm_mon  = (int)v[1];            /* 0-based month */
        tm.tm_mday = (argc > 2) ? (int)v[2] : 1;
        tm.tm_hour = (int)v[3];
        tm.tm_min  = (int)v[4];
        tm.tm_sec  = (int)v[5];
        tm.tm_isdst = -1;                  /* let mktime resolve DST */
        time_t t = mktime(&tm);
        ms = (int64_t)t * 1000 + v[6];
    } else if(argc == 1) {
        var_t* arg = get_func_arg(env, 0);
        if(arg != NULL && arg->type == V_STRING) {
            if(!js_date_parse_iso(var_get_str(arg), &ms)) {
                const char* sv = var_get_str(arg);
                if(sv == NULL || *sv == 0)
                    ms = JS_DATE_INVALID;   /* Date.parse("") = NaN -> Invalid Date;
                                             * vm_to_number("") would wrongly give 0. */
                else
                    ms = js_date_to_ms(vm, arg);   /* numeric-looking string; NaN-safe */
            }
        } else if(arg != NULL && arg->type != V_UNDEF) {
            /* Covers numbers AND objects. An object must go through ToPrimitive
             * (vm_to_number) per `new Date(obj)` semantics; var_get_float() would
             * read obj->value as a raw float, which is both wrong (an object's
             * ->value is internal data, not a number) and unsafe - crash#1's
             * var_get_float fault was exactly such a dangling ->value deref.
             * vm_to_number also keeps full double precision where var_get_float's
             * 32-bit float truncated large epoch-ms values. */
            ms = js_date_to_ms(vm, arg);
        } else {
            /* One actual argument that IS undefined: ToNumber(undefined) = NaN =
             * Invalid Date (only the ZERO-arg form means "now"). */
            ms = JS_DATE_INVALID;
        }
    } else {
        ms = js_now_ms();                   /* new Date(): precise wall clock */
    }
    obj->value = (void*)(intptr_t)ms;
    obj->free_func = js_date_free;
    js_date_stamp_mario(vm, obj, ms);
    return NULL;   /* new_obj keeps `this` when the ctor returns a non-object */
}

static var_t* js_date_get_time(vm_t* vm, var_t* env, void* data) {
    (void)data;
    /* A wall-clock stamp needs more than float32 mantissa (it is ~1.7e12 ms,
     * i.e. beyond 2^24), so the canonical double type keeps it exact. */
    int64_t ms = js_date_ms_of(this_from_env(env));
    if(ms == JS_DATE_INVALID)
        return var_new_float64(vm, NAN);
    return var_new_float64(vm, (double)ms);
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
    if(ms == JS_DATE_INVALID)
        return var_new_float64(vm, NAN);
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
    int64_t ms;
    if(arg != NULL && arg->type == V_STRING) {
        if(!js_date_parse_iso(var_get_str(arg), &ms))
            ms = js_date_to_ms(vm, arg);
    } else {
        ms = (arg != NULL && arg->type != V_UNDEF) ? js_date_to_ms(vm, arg) : JS_DATE_INVALID;
    }
    obj->value = (void*)(intptr_t)ms;
    obj->free_func = js_date_free;
    js_date_stamp_mario(vm, obj, ms);
    if(ms == JS_DATE_INVALID)
        return var_new_float64(vm, NAN);
    return var_new_float64(vm, (double)ms);
}

/* toLocaleDateString/toLocaleString/toLocaleTimeString: mario's native_Date.c
 * maps these to its full toString format, but github's file list calls
 * toLocaleDateString("en-US", {month:"short",day:"numeric",year:"numeric"}) and
 * expects "Sep 5, 2026". Honour the common en-US option subset here. */
static const char* kMoShort[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
static const char* kMoLong[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};

static const char* js_date_opt_str(var_t* opts, const char* key) {
    if(opts == NULL || opts->type != V_OBJECT) return NULL;
    node_t* n = var_find_own_member(opts, key);
    if(n == NULL || n->var == NULL || n->var->type != V_STRING) return NULL;
    return var_get_str(n->var);
}

/* which: 0=date only, 1=date+time, 2=time only */
static var_t* js_date_to_locale(vm_t* vm, var_t* env, int which) {
    int64_t ms = js_date_ms_of(this_from_env(env));
    if(ms == JS_DATE_INVALID)
        return var_new_str(vm, "Invalid Date");
    time_t sec = (time_t)(ms / 1000);
    struct tm t;
    memset(&t, 0, sizeof(t));
    localtime_r(&sec, &t);
    var_t* opts = get_obj(env, "options");
    /* `toLocaleDateString("zh-CN")` passes the LOCALE as arg0 and leaves options
     * undefined; get_obj still hands back the undefined var (non-NULL), so the
     * old `opts == NULL` test skipped the default fill and every field stayed
     * NULL -> an EMPTY string (a React #418 text mismatch on rokid.com). Treat
     * any non-object as "no options". */
    int had_opts = (opts != NULL && opts->type == V_OBJECT);
    const char* mo = js_date_opt_str(opts, "month");
    const char* dy = js_date_opt_str(opts, "day");
    const char* yr = js_date_opt_str(opts, "year");
    const char* hr = js_date_opt_str(opts, "hour");
    const char* mi = js_date_opt_str(opts, "minute");
    const char* sd = js_date_opt_str(opts, "second");
    bool want_date = (which == 0 || which == 1);
    bool want_time = (which == 1 || which == 2);
    if(!had_opts) {   /* default en-US: M/D/YYYY, h:mm:ss AM/PM */
        mo = "numeric"; dy = "numeric"; yr = "numeric";
        hr = "numeric"; mi = "2-digit"; sd = "2-digit";
        want_date = true; want_time = true;
    }
    /* zh layout: option-driven fields glue with 年/月/日; the no-option default
     * is the slash form. Time is a 24-hour clock. */
    var_t* locv = get_obj(env, "locale");
    const char* loc = (locv != NULL && locv->type == V_STRING) ? var_get_str(locv) : "";
    if(loc != NULL && loc[0] == 'z' && loc[1] == 'h') {
        char buf[96]; buf[0] = 0;
        if(want_date) {
            if(had_opts && (mo != NULL || dy != NULL || yr != NULL)) {
                if(yr != NULL) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d年", t.tm_year + 1900);
                if(mo != NULL) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d月", t.tm_mon + 1);
                if(dy != NULL) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d日", t.tm_mday);
            } else {
                snprintf(buf, sizeof(buf), "%04d/%02d/%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
            }
        }
        if(want_time) {
            char tp[40];
            if(hr != NULL && mi != NULL && sd != NULL)
                snprintf(tp, sizeof(tp), "%s%02d:%02d:%02d", buf[0] ? " " : "", t.tm_hour, t.tm_min, t.tm_sec);
            else if(hr != NULL && mi != NULL)
                snprintf(tp, sizeof(tp), "%s%02d:%02d", buf[0] ? " " : "", t.tm_hour, t.tm_min);
            else if(hr != NULL)
                snprintf(tp, sizeof(tp), "%s%02d", buf[0] ? " " : "", t.tm_hour);
            else tp[0] = 0;
            strcat(buf, tp);
        }
        return var_new_str(vm, buf);
    }
    char buf[96]; buf[0] = 0;
    if(want_date) {
        char part[48]; part[0] = 0;
        if(mo != NULL) {
            if(strcmp(mo, "short") == 0) snprintf(part, sizeof(part), "%s", kMoShort[t.tm_mon % 12]);
            else if(strcmp(mo, "long") == 0) snprintf(part, sizeof(part), "%s", kMoLong[t.tm_mon % 12]);
            else snprintf(part, sizeof(part), "%d", t.tm_mon + 1);
        }
        if(dy != NULL) {
            char d2[8];
            if(strcmp(dy, "2-digit") == 0) snprintf(d2, sizeof(d2), "%02d", t.tm_mday);
            else snprintf(d2, sizeof(d2), "%d", t.tm_mday);
            if(part[0]) { strcat(part, " "); strcat(part, d2); } else snprintf(part, sizeof(part), "%s", d2);
        }
        if(yr != NULL) {
            char y2[8];
            if(strcmp(yr, "2-digit") == 0) snprintf(y2, sizeof(y2), "%02d", (t.tm_year + 1900) % 100);
            else snprintf(y2, sizeof(y2), "%d", t.tm_year + 1900);
            if(part[0]) { strcat(part, ", "); strcat(part, y2); } else snprintf(part, sizeof(part), "%s", y2);
        }
        strcat(buf, part);
    }
    if(want_time) {
        char tp[40];
        int h = t.tm_hour;
        const char* ap = "";
        if(hr != NULL) {   /* en-US defaults to 12-hour clock */
            ap = (h >= 12) ? " PM" : " AM";
            h = h % 12; if(h == 0) h = 12;
        }
        if(hr != NULL && mi != NULL && sd != NULL)
            snprintf(tp, sizeof(tp), "%s%d:%02d:%02d%s", (buf[0] ? ", " : ""), h, t.tm_min, t.tm_sec, ap);
        else if(hr != NULL && mi != NULL)
            snprintf(tp, sizeof(tp), "%s%d:%02d%s", (buf[0] ? ", " : ""), h, t.tm_min, ap);
        else if(hr != NULL)
            snprintf(tp, sizeof(tp), "%s%d%s", (buf[0] ? ", " : ""), h, ap);
        else if(mi != NULL)
            snprintf(tp, sizeof(tp), "%s%02d", (buf[0] ? ", " : ""), t.tm_min);
        else tp[0] = 0;
        strcat(buf, tp);
    }
    return var_new_str(vm, buf);
}

static var_t* js_date_to_locale_date(vm_t* vm, var_t* env, void* data) { (void)data; return js_date_to_locale(vm, env, 0); }
static var_t* js_date_to_locale_both(vm_t* vm, var_t* env, void* data) { (void)data; return js_date_to_locale(vm, env, 1); }
static var_t* js_date_to_locale_time(vm_t* vm, var_t* env, void* data) { (void)data; return js_date_to_locale(vm, env, 2); }

/* "Wed, 21 Oct 2015 07:28:00 GMT" - toGMTString is the legacy alias of
 * toUTCString; both appear in cookie and analytics code. */
static var_t* js_date_to_gmt_string(vm_t* vm, var_t* env, void* data) {
    (void)data;
    int64_t ms = js_date_ms_of(this_from_env(env));
    if(ms == JS_DATE_INVALID)
        return var_new_str(vm, "Invalid Date");
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
    vm_reg_native(vm, cls, "constructor(a,b,c,d,e,f,g)", js_date_ctor, NULL);
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
    vm_reg_native(vm, cls, "toLocaleDateString(locale, options)", js_date_to_locale_date, NULL);
    vm_reg_native(vm, cls, "toLocaleString(locale, options)", js_date_to_locale_both, NULL);
    vm_reg_native(vm, cls, "toLocaleTimeString(locale, options)", js_date_to_locale_time, NULL);
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

static int js_add_timer(vm_t* vm, js_dom_state* st, var_t* cb, uint32_t ms, bool repeat, bool microtask, bool idle) {
    int slot = -1;
    for(int i = 0; i < JS_TIMER_MAX; ++i)
        if(!st->timers[i].active) { slot = i; break; }
    if(slot < 0) {                                /* table full: report failure */
        if(getenv("MARIO_TIMERDBG") != NULL)
            fprintf(stderr, "[timerdbg] ADD_FAIL table full cb=%p\n", (void*)cb);
        /* Overflow here silently DROPS the callback (see JS_TIMER_MAX comment),
         * which can strand an event loop (e.g. React's scheduler MessageChannel
         * message never fires and an idle render never reaches its fiber). Keep
         * an always-on, rate-limited warning so a drop is never invisible; the
         * cumulative count is echoed by the MARIO_MTDRAIN poll diagnostic. */
        st->timer_drops++;
        if(st->timer_drops <= 5 || (st->timer_drops % 256) == 0)
            fprintf(stderr, "[jsdom] timer table full (%d slots): DROPPED %ld callback(s) so far\n",
                JS_TIMER_MAX, (long)st->timer_drops);
        return 0;
    }
    st->timer_next_id++;
    if(st->timer_next_id <= 0) st->timer_next_id = 1;
    js_timer_t* t = &st->timers[slot];
    t->id           = st->timer_next_id;
    t->cb           = cb;
    t->microtask    = microtask;
    t->idle         = idle;
    if(microtask) {
        /* Due on the very next poll (remaining_ms 0) and never repeats: a promise
         * reaction / queueMicrotask / process.nextTick runs once, ahead of any
         * 0-ms macrotask, then frees its slot. period_ms 0 keeps js_fire_timer on
         * the one-shot path (repeat forced false). */
        t->period_ms    = 0;
        t->remaining_ms = 0;
        t->repeat       = false;
    } else {
        t->period_ms    = (ms < 1) ? 1 : ms;
        t->remaining_ms = (int64_t)t->period_ms;
        t->repeat       = repeat;
    }
    t->active       = true;
    if(getenv("MARIO_TIMERDBG") != NULL)
        fprintf(stderr, "[timerdbg] add id=%d cb=%p ms=%u repeat=%d micro=%d\n",
            t->id, (void*)cb, (unsigned)ms, repeat ? 1 : 0, microtask ? 1 : 0);
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
    return var_new_int(vm, js_add_timer(vm, st, cn->var, ms, repeat, false, false));
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

/* IdleDeadline.timeRemaining(): ms left in this idle period. A constant 0 made
 * every idle-work loop (`while (deadline.timeRemaining() > 0) doWork()`) run
 * zero iterations and re-schedule forever - a silent stall. Real browsers hand
 * out ~50ms that drains as wall time passes, so record the deadline's start on
 * the object and drain from there; a synchronous loop sees the full budget and
 * finishes its work, a long one eventually yields. */
#define IDLE_BUDGET_MS 50
static var_t* native_idle_timeRemaining(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = get_obj(env, THIS);
    int64_t start = 0;
    bool have = false;
    if(self != NULL) {
        var_t* s = var_find_own_member_var(self, "@@idle_start");
        if(s != NULL) { start = var_get_int64(s); have = true; }
    }
    if(!have) return var_new_int(vm, IDLE_BUDGET_MS);   /* unbound this: full budget */
    int64_t rem = IDLE_BUDGET_MS - (js_now_ms() - start);
    if(rem < 0) rem = 0;
    return var_new_int(vm, (int)rem);
}

/* window.requestIdleCallback(cb[, {timeout}]): schedule cb as a short
 * macrotask and hand it an IdleDeadline. Bundles that feature-detect the API
 * (`"requestIdleCallback" in globalThis`) otherwise fall back to setTimeout
 * with no deadline argument at all, which is fine for them but leaves the
 * detection lying; a real implementation keeps both paths honest. */
static var_t* native_requestIdleCallback(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    node_t* cn = var_array_get(args, 0);
    if(st == NULL || cn == NULL || cn->var == NULL || !cn->var->is_func)
        return var_new_int(vm, 0);
    return var_new_int(vm, js_add_timer(vm, st, cn->var, 1, false, false, true));
}

static var_t* native_cancelIdleCallback(vm_t* vm, var_t* env, void* data) {
    return js_clear_timer_native(vm, env, data);
}

/* ------------------------------------------------------------------ */
/* document.fonts (FontFaceSet)                                       */
/*                                                                     */
/* Fonts are resolved synchronously by the porting layer while laying    */
/* out (a @font-face rule with a downloaded src is available as soon as */
/* the stylesheet is), so there is never a pending font load to wait     */
/* for: `document.fonts.ready` is an already-resolved promise and        */
/* check()/load() report success immediately. Without the object a page  */
/* that awaits document.fonts.ready before its first paint waits on a    */
/* TypeError instead.                                                   */
/* ------------------------------------------------------------------ */

#define DOM_FONTS_KEY "@@fonts"

/* vm_reg_static's is_static flag is inert: the engine registers the function
 * on the class PROTOTYPE and relies on member lookup falling through to it,
 * so a C-side call has to do that lookup by hand. */
static var_t* dom_static_func(vm_t* vm, const char* cls_name, const char* func_name) {
    var_t* cls = var_find_own_member_var(vm->root, cls_name);
    if(cls == NULL) return NULL;
    var_t* fn = NULL;
    var_t* proto = var_get_prototype(cls);
    if(proto != NULL) fn = get_obj(proto, func_name);
    if(fn == NULL || !fn->is_func) fn = get_obj(cls, func_name);
    return (fn != NULL && fn->is_func) ? fn : NULL;
}

/* Promise.resolve(value) / Promise.reject(reason). Returns an OWNED var (the
 * caller hands it straight back as the native's result) or NULL when the
 * engine has no Promise, so callers can fall back to the bare value. */
static var_t* dom_promise(vm_t* vm, const char* which, var_t* value) {
    var_t* cls = var_find_own_member_var(vm->root, "Promise");
    var_t* fn = dom_static_func(vm, "Promise", which);
    if(cls == NULL || fn == NULL) return NULL;
    var_t* args = var_new_array(vm);
    var_array_add(args, value);
    var_t* p = call_m_func(vm, cls, fn, args);
    var_unref(args);
    return p;
}

/* FontFaceSet: an EventTarget-like set that is always empty and always
 * loaded. The members pages actually touch are ready/status/check/load and
 * the iteration helpers (it is a set-like, so forEach is the common path). */
static var_t* native_fonts_ready(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    var_t* p = dom_promise(vm, "resolve", NULL);
    return (p != NULL) ? p : var_new_null(vm);
}
static var_t* native_fonts_check(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    /* No font is ever "still loading", so any query is answered yes. */
    return var_new_bool(vm, true);
}
static var_t* native_fonts_load(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* arr = var_new_array(vm);           /* zero FontFaces needed */
    var_t* p = dom_promise(vm, "resolve", arr);
    return (p != NULL) ? p : arr;
}
static var_t* native_fonts_size(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_int(vm, 0);
}
static var_t* native_fonts_noop(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return NULL;
}
static var_t* native_fonts_forEach(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;                    /* empty set: nothing to visit */
    return NULL;
}
static var_t* native_fonts_iterator(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_array(vm);                 /* for..of over an empty set */
}

/* new FontFace(family, source[, descriptors]): the object is inert (nothing
 * loads it into the layout font set) but constructing one and awaiting
 * `face.load()` must not throw, which is how font preloading snippets probe
 * for support. */
static var_t* native_FontFace_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = get_obj(env, THIS);
    var_t* args = get_func_args(env);
    node_t* fn = var_array_get(args, 0);
    node_t* sn = var_array_get(args, 1);
    if(self == NULL) return NULL;
    var_add(self, "family", var_new_str(vm, (fn != NULL && fn->var != NULL) ? var_get_str(fn->var) : ""));
    var_add(self, "source",  var_new_str(vm, (sn != NULL && sn->var != NULL) ? var_get_str(sn->var) : ""));
    var_add(self, "status",  var_new_str(vm, "loaded"));
    var_add(self, "loaded",  native_fonts_ready(vm, env, data));
    return NULL;
}

static var_t* native_FontFace_load(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = get_obj(env, THIS);
    var_t* p = dom_promise(vm, "resolve", self);
    return (p != NULL) ? p : self;
}

/* document.fonts: build the singleton once and root it on the bridge var so
 * the GC can not sweep it between script runs. */
static var_t* native_document_get_fonts(vm_t* vm, var_t* env, void* data) {
    (void)env;
    var_t* bridge = (var_t*)data;
    if(bridge == NULL) bridge = var_find_own_member_var(vm->root, DOM_BRIDGE_KEY);
    if(bridge == NULL) return NULL;
    var_t* fonts = var_find_own_member_var(bridge, DOM_FONTS_KEY);
    if(fonts != NULL) return fonts;
    fonts = var_new_obj(vm, var_get_prototype(vm->builtin_vars.var_Object), NULL, NULL);
    if(fonts == NULL) return NULL;
    vm->gc.gc_defer++;
    var_add(fonts, "status", var_new_str(vm, "loaded"));
    vm_reg_native_on(vm, fonts, "check(f, t)",   native_fonts_check,    NULL);
    vm_reg_native_on(vm, fonts, "load(f, t)",    native_fonts_load,     NULL);
    vm_reg_native_on(vm, fonts, "add(face)",     native_fonts_noop,     NULL);
    vm_reg_native_on(vm, fonts, "delete(face)",  native_fonts_check,    NULL);   /* true */
    vm_reg_native_on(vm, fonts, "clear()",       native_fonts_noop,     NULL);
    vm_reg_native_on(vm, fonts, "forEach(f)",    native_fonts_forEach,  NULL);
    vm_reg_native_on(vm, fonts, "entries()",     native_fonts_iterator, NULL);
    vm_reg_native_on(vm, fonts, "keys()",        native_fonts_iterator, NULL);
    vm_reg_native_on(vm, fonts, "values()",      native_fonts_iterator, NULL);
    vm_reg_native_on(vm, fonts, "addEventListener(t, f)",    native_fonts_noop, NULL);
    vm_reg_native_on(vm, fonts, "removeEventListener(t, f)", native_fonts_noop, NULL);
    vm_reg_native_on(vm, fonts, "dispatchEvent(e)",          native_fonts_check, NULL);
    /* `ready` is a getter in the spec and is re-read after every load, so it
     * has to hand back a FRESH resolved promise each time. */
    js_acc_on(vm, fonts, "ready", native_fonts_ready, NULL, NULL);
    js_acc_on(vm, fonts, "size",  native_fonts_size,  NULL, NULL);
    var_add(bridge, DOM_FONTS_KEY, fonts);
    vm->gc.gc_defer--;
    return fonts;
}

/* Fire one due timer slot: a repeat timer re-arms for the next period (one fire
 * per poll, no catch-up burst); a one-shot (incl. every microtask) is deactivated
 * and its cb cleared. The callback then runs. Returns 1 when a function actually
 * ran, 0 when the slot held no callable. The fired one-shot stays anchored in the
 * OLD @@timers array until the caller's js_reanchor_timers, so the GC can not
 * sweep its callback mid-call. */
static int js_fire_timer(vm_t* vm, js_dom_state* st, int idx) {
    js_timer_t* t = &st->timers[idx];
    var_t* cb = t->cb;
    if(t->repeat)
        t->remaining_ms = (int64_t)t->period_ms;
    else {
        t->active = false;
        t->cb = NULL;
    }
    if(cb == NULL || !cb->is_func) return 0;
    if(getenv("MARIO_TIMERDBG") != NULL) {
        func_t* cf = (func_t*)cb->value;
        fprintf(stderr, "[timerdbg] fire id=%d cb=%p micro=%d entrypc=%u native=%d scope_top=%d stack_top=%d call_depth=%d\n",
            t->id, (void*)cb, t->microtask ? 1 : 0, (unsigned)(cf != NULL ? cf->pc : 0),
            (cf != NULL && cf->native != NULL) ? 1 : 0,
            (int)vm->scope_stack_top, (int)vm->stack_top, (int)vm->call_depth);
    }
    var_t* args = var_new_array(vm);
    if(t->idle) {
        /* IdleDeadline for requestIdleCallback callbacks (see js_timer_t.idle).
         * Ownership follows the bridge convention (see js_reanchor_timers and the
         * io_schedule note): var_new_* returns refs==0 and var_add/var_array_add
         * takes the ONLY reference, so the parent keeps the child alive and an
         * explicit var_unref here would drop refs to 0 and var_free() a var the
         * parent node still points at. args itself is unref'd below, which is a
         * no-op at refs==0 and only releases it if call_m_func took a reference. */
        var_t* dl = var_new_obj(vm, var_get_prototype(vm->builtin_vars.var_Object), NULL, NULL);
        var_add(dl, "didTimeout", var_new_bool(vm, false));
        var_add(dl, "timeRemaining", var_new_native_func(vm, native_idle_timeRemaining, NULL));
        var_add(dl, "@@idle_start", var_new_int64(vm, js_now_ms()));
        var_array_add(args, dl);
    }
    /* Each timer/microtask callback is an independent JOB: per the HTML/ES event
     * loop a task runs to completion and an error that escapes it is reported to
     * the global handler and then CLEARED - it must never leak into the next job.
     * mario propagates an uncaught throw via vm->propagating_err + vm->abort_run,
     * and call_m_func -> vm_run aborts immediately when abort_run is already set.
     * Without this isolation an uncaught throw in one microtask (e.g. a page
     * helper doing `for(t of e) t.getAttribute(...)`) left abort_run set, so the
     * NEXT job - a webpack lazy-chunk registration reaction `import(x).then(d)` -
     * bailed before d's body ran, its factories never merged into require.m, and
     * the page stayed a grey skeleton. Report+clear before (defensive: a leftover
     * from any entry point) and after (this job's own escape). */
    if(vm->propagating_err != NULL)
        vm_report_uncaught(vm);
    var_t* r = call_m_func(vm, NULL, cb, args);
    if(vm->propagating_err != NULL)
        vm_report_uncaught(vm);
    if(r != NULL) var_unref(r);
    var_unref(args);
    return 1;
}

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

    /* Phase 2: fire due timers, MICROTASKS FIRST. The spec runs a microtask
     * checkpoint after every macrotask, so drain EVERY due microtask (promise
     * reaction / queueMicrotask / process.nextTick) before firing a single
     * macrotask (setTimeout / MessageChannel), then loop back to drain again - a
     * macrotask or reaction may queue more microtasks, which must run before the
     * next macrotask. Microtasks are picked in id (insertion) order so promise
     * chains run FIFO; macrotasks keep the old lowest-slot scan. iters (not fires)
     * is bounded so a slot holding no callable still progresses toward the break.
     * Without this priority the engine fired 0-ms macrotasks ahead of promise
     * reactions, inverting the spec ordering and deadlocking React 18's
     * concurrent scheduler (its MessageChannel work loop ran before the flight
     * ping microtask, re-suspended, then reused a spent task forever). */
    int fired = 0;
    int iters = 0;
    int n_micro = 0, n_macro = 0;
    const int ITER_CAP = 100000;
    while(iters++ < ITER_CAP) {
        int due = -1, best = 0;
        for(int i = 0; i < JS_TIMER_MAX; ++i)
            if(st->timers[i].active && st->timers[i].microtask && st->timers[i].remaining_ms <= 0 &&
               (due < 0 || st->timers[i].id < best)) { due = i; best = st->timers[i].id; }
        if(due >= 0) { fired += js_fire_timer(vm, st, due); n_micro++; continue; }
        due = -1;
        for(int i = 0; i < JS_TIMER_MAX; ++i)
            if(st->timers[i].active && !st->timers[i].microtask && st->timers[i].remaining_ms <= 0) { due = i; break; }
        if(due < 0) break;
        fired += js_fire_timer(vm, st, due); n_macro++;
    }
    /* DIAG (temp, MARIO_MTDRAIN): confirm/deny microtask starvation of the
     * macrotask queue. capHit means the drain loop ran to ITER_CAP without
     * exhausting due microtasks - if that repeats every poll, macrotasks
     * (React's scheduler MessageChannel, page setTimeout) never run. actMicro
     * shows the live microtask backlog (table pressure). Remove with probes. */
    if(getenv("MARIO_MTDRAIN") != NULL) {
        bool capped = (iters > ITER_CAP);
        int act_micro = 0, act_macro = 0;
        for(int i = 0; i < JS_TIMER_MAX; ++i)
            if(st->timers[i].active) { if(st->timers[i].microtask) act_micro++; else act_macro++; }
        static long c_micro = 0, c_macro = 0, c_cap = 0, c_poll = 0;
        static uint64_t s_last = 0;
        c_micro += n_micro; c_macro += n_macro; c_poll++;
        if(capped) c_cap++;
        uint64_t nm = js_dom_monotonic_ms();
        if(capped || nm - s_last >= 500) {
            s_last = nm;
            fprintf(stderr, "[mtdrain] poll=%ld this(micro=%d macro=%d capped=%d) act(micro=%d macro=%d) cum(micro=%ld macro=%ld capHit=%ld) drops=%ld\n",
                c_poll, n_micro, n_macro, capped ? 1 : 0, act_micro, act_macro, c_micro, c_macro, c_cap, (long)st->timer_drops);
            /* Stuck-timer detail: any ACTIVE slot already due (remaining<=0)
             * that the phase-2 scan should have fired this poll. Distinguishes
             * a corrupted remaining_ms (never due) from a corrupted cb (fires
             * as a no-op) from a slot the scan itself skips. */
            for(int i = 0; i < JS_TIMER_MAX; ++i) {
                if(st->timers[i].active &&
                   (st->timers[i].remaining_ms <= 0 ||
                    (!st->timers[i].repeat && st->timers[i].period_ms <= 1 &&
                     st->timers[i].remaining_ms > 1000))) {
                    var_t* tcb = st->timers[i].cb;
                    fprintf(stderr, "[mtstuck] slot=%d id=%d micro=%d repeat=%d rem=%lld per=%u cb=%p isfunc=%d\n",
                        i, st->timers[i].id, st->timers[i].microtask ? 1 : 0,
                        st->timers[i].repeat ? 1 : 0,
                        (long long)st->timers[i].remaining_ms,
                        (unsigned)st->timers[i].period_ms,
                        (void*)tcb, (tcb != NULL && tcb->is_func) ? 1 : 0);
                }
            }
        }
    }
    js_reanchor_timers(vm, st);   /* drop fired one-shots from the GC anchor */
    return fired;
}

/* Non-zero when at least one timer (incl. a queued MessageChannel post, which
 * rides this table at 0 ms) is still armed. The await-spin hook uses it to tell
 * "idle but more work coming" from "event loop empty, nothing can settle the
 * awaited promise". */
int js_dom_has_pending_timers(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    if(vm == NULL || st == NULL) return 0;
    for(int i = 0; i < JS_TIMER_MAX; ++i)
        if(st->timers[i].active) return 1;
    return 0;
}

/* Non-zero when a ONE-SHOT timer is armed to fire on essentially the next poll
 * - a 0-ms macrotask such as React's MessageChannel scheduler continuation,
 * which js_add_timer stores with remaining_ms 1 (ms<1 -> period 1). The engine
 * uses this to DEFER a repaint: painting between back-to-back scheduler chunks
 * costs a full repaint of a large DOM after every ~5 ms slice, whereas a browser
 * runs the message-task chain and paints only once it drains. Repeating timers
 * (setInterval) and long-delayed one-shots (setTimeout(N), a deferred fetch)
 * keep remaining_ms >> 1 and never suppress the paint. */
int js_dom_has_imminent_timers(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    if(vm == NULL || st == NULL) return 0;
    for(int i = 0; i < JS_TIMER_MAX; ++i)
        if(st->timers[i].active && !st->timers[i].repeat &&
           st->timers[i].remaining_ms <= 1) return 1;
    return 0;
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
        /* EWEB_IMGDBG: count createElement('img') so a React-rendered <img>
         * (cover-image, which sets src via property not setAttribute) is
         * visible even when no setAttribute/src=string probe fires. */
        {
            static int s_imgcedbg = -1;
            if(s_imgcedbg < 0) { const char* e = getenv("EWEB_IMGDBG"); s_imgcedbg = (e && e[0] == '1') ? 1 : 0; }
            if(s_imgcedbg && strcmp(low->cstr, "img") == 0) {
                static int s_imgce = 0;
                s_imgce++;
                fprintf(stderr, "[IMGDBG] createElement img #%d el=%p\n", s_imgce, (void*)el);
            }
        }
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

/* DocumentFragment wrapper bound to an element handle. Used for
 * HTMLTemplateElement.content: the engine has no separate fragment node, so the
 * fragment shares the template element's handle and its mutation/query natives
 * read/write the template's children (see native_el_get_content). */
static var_t* wrap_fragment(vm_t* vm, js_element_t el) {
    var_t* o = new_obj(vm, CLS_FRAGMENT, 0);
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
    /* apple.com.cn's locale router iterates querySelectorAll('[hreflang]') and
     * calls el.hreflang.split('-'); without the reflection the GET yields a
     * non-string and the split throws, aborting hydration of the main tree. */
    {"hreflang",    "hreflang"},
    /* `content` is NOT here: HTMLTemplateElement.content is a DocumentFragment,
     * not the reflected attribute, so it gets a dedicated branching accessor
     * (native_el_get_content) registered after this loop. Every other element
     * (meta/param) still reflects the attribute through that accessor. */
    {"charset",     "charset"},
    {"httpEquiv",   "http-equiv"},
    {"media",       "media"},
    {"target",      "target"},
    {"nonce",       "nonce"},
    {"crossOrigin", "crossorigin"},
    {"referrerPolicy", "referrerpolicy"},
    {"srcset",      "srcset"},
    {"sizes",       "sizes"},
    {"role",        "role"},
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
    /* EWEB_IMGDBG: the React cover-image <img> sets src via the reflected
     * property (el.src=), which lands here - NOT in native_el_setAttribute. */
    {
        static int s_propsrcdbg = -1;
        if(s_propsrcdbg < 0) { const char* e = getenv("EWEB_IMGDBG"); s_propsrcdbg = (e && e[0] == '1') ? 1 : 0; }
        if(s_propsrcdbg && v != NULL && (strstr(v, "1200x") != NULL || strstr(v, "control1") != NULL))
            fprintf(stderr, "[IMGDBG] coverimg prop-src %s=%.80s el=%p\n", kAttrProps[idx].attr, v, (void*)el);
    }
    st->cb.el_set_attr(st->ctx, el, kAttrProps[idx].attr, v);
    mstr_free(s);
    return NULL;
}

/* Element.content: for a <template> this is the DocumentFragment holding the
 * template's subtree; for every other element (meta/param) it is the reflected
 * `content` attribute. Browsers keep a template's children in a separate
 * fragment node, but this engine has no distinct fragment, so the wrapper is
 * bound to the SAME template handle - appendChild/cloneNode/querySelector on it
 * mutate/read the template's children. github's task-lists drag-handle builder
 * runs `h.content.appendChild(m)` at module top level; with `.content` treated
 * purely as an attribute it returned "" and threw "can not find function
 * 'appendChild'" (recv_type V_STRING), aborting the whole jz5 module. */
static var_t* native_el_get_content(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st != NULL && el != NULL && st->cb.el_get_tag != NULL) {
        char* tag = st->cb.el_get_tag(st->ctx, el);
        bool is_tpl = false;
        if(tag != NULL) {
            is_tpl = (strcmp(tag, "template") == 0);
            mario_free(tag);
        }
        if(is_tpl)
            return wrap_fragment(vm, el);
    }
    /* Not a template: reflect the `content` attribute. */
    if(st == NULL || el == NULL || st->cb.el_get_attr == NULL)
        return var_new_str(vm, "");
    char* v = st->cb.el_get_attr(st->ctx, el, "content");
    var_t* r = var_new_str(vm, (v != NULL) ? v : "");
    if(v != NULL) mario_free(v);
    return r;
}

static var_t* native_el_set_content(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_set_attr == NULL) return NULL;
    mstr_t* s = mstr_new("");
    const char* v = js_arg_cstr(env, 0, s);
    st->cb.el_set_attr(st->ctx, el, "content", v);
    mstr_free(s);
    return NULL;
}

/* ---- Shadow DOM (degenerate, light-DOM backed) ----------------------------
 * The headless renderer models no separate shadow tree, but real pages wrap and
 * call these unconditionally: github's catalyst patches
 * `Element.prototype.attachShadow = function(e){ let o = n.call(this,e); ... }`
 * at module load and invokes `this.attachShadow({mode})` from connectedCallback.
 * With the natives absent, `n` (the saved original) was undefined and the wrapper
 * threw "can not find function 'call'", aborting the upgrade of every element that
 * opens a shadow root. Browsers return a ShadowRoot that is rendered in place, so
 * the closest faithful behaviour here is a DocumentFragment wrapper bound to the
 * SAME element handle (wrap_fragment): content appended to the returned root is
 * laid out and painted as light-DOM children, and `shadowRoot` reads it back. */
#define SHADOW_ROOT_KEY "@@shadowroot"

static var_t* native_el_noop_null(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;
}
static var_t* native_el_noop_true(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    return var_new_bool(vm, true);
}

/* Return (and memoise) the element's shadow root wrapper. The memo lives in a
 * hidden own member so repeat attachShadow() calls and the shadowRoot getter
 * agree; the member holds the only persistent ref, and every hand-out takes a
 * fresh ref so the caller's release never frees the memoised wrapper. */
static var_t* el_shadow_root(vm_t* vm, var_t* self, bool create) {
    if(self == NULL) return var_new_null(vm);
    var_t* root = var_find_own_member_var(self, SHADOW_ROOT_KEY);
    if(root != NULL) { var_ref(root); return root; }
    if(!create) return var_new_null(vm);
    js_element_t el = handle_from_this(self);
    if(el == NULL) return var_new_null(vm);
    root = wrap_fragment(vm, el);
    if(root == NULL || root->type != V_OBJECT) return var_new_null(vm);
    var_add(self, SHADOW_ROOT_KEY, root);   /* member takes the persistent ref */
    var_ref(root);                          /* + the ref we hand to the caller */
    return root;
}

static var_t* native_el_attachShadow(vm_t* vm, var_t* env, void* data) {
    (void)data;
    return el_shadow_root(vm, this_from_env(env), true);
}

static var_t* native_el_get_shadowRoot(vm_t* vm, var_t* env, void* data) {
    (void)data;
    return el_shadow_root(vm, this_from_env(env), false);
}

/* ElementInternals: catalyst reads `internals.shadowRoot` right after
 * attachInternals(); the remaining members are no-ops so feature-detects pass. */
static var_t* native_el_attachInternals(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = this_from_env(env);
    var_t* oi = var_new_obj(vm, var_get_prototype(vm->builtin_vars.var_Object), NULL, NULL);
    if(oi == NULL) return var_new_null(vm);
    var_add(oi, "shadowRoot", el_shadow_root(vm, self, false));
    var_add(oi, "form", var_new_null(vm));
    var_add(oi, "validity", var_new_obj(vm, var_get_prototype(vm->builtin_vars.var_Object), NULL, NULL));
    var_add(oi, "validationMessage", var_new_str(vm, ""));
    var_add(oi, "willValidate", var_new_bool(vm, false));
    var_add(oi, "labels", var_new_array(vm));
    var_add(oi, "setFormValue(v, s)", var_new_native_func(vm, native_el_noop_null, NULL));
    var_add(oi, "setValidity(f, m, a)", var_new_native_func(vm, native_el_noop_null, NULL));
    var_add(oi, "checkValidity()", var_new_native_func(vm, native_el_noop_true, NULL));
    var_add(oi, "reportValidity()", var_new_native_func(vm, native_el_noop_true, NULL));
    return oi;   /* refs==0: the members above each hold their own ref */
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

/* Element.toggleAttribute(name[, force]): github-elements' constructors flip
 * boolean attributes (hidden, open, disabled) through it; without it the very
 * first upgrade throws and the whole registration chain aborts. */
static var_t* native_el_toggleAttribute(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    var_t* fv = js_arg(env, 1);
    bool has = false;
    if(st != NULL && el != NULL && name[0] != 0 && st->cb.el_get_attr != NULL) {
        char* v = st->cb.el_get_attr(st->ctx, el, name);
        has = (v != NULL);
        if(v != NULL) mario_free(v);
    }
    bool want = (fv != NULL && (fv->type == V_BOOL || fv->type == V_INT))
                    ? var_get_bool(fv) : !has;
    if(st != NULL && el != NULL && name[0] != 0) {
        if(want) {
            if(st->cb.el_set_attr != NULL && !has)
                st->cb.el_set_attr(st->ctx, el, name, "");
        }
        else if(st->cb.el_remove_attr != NULL && has)
            st->cb.el_remove_attr(st->ctx, el, name);
    }
    mstr_free(s);
    return var_new_bool(vm, want);
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

static var_t* native_el_get_firstChild(vm_t* vm, var_t* env, void* data)        { var_t* r = edge_child(vm, env, data, true, false); js_dom_state* s0 = state_any(vm, data); js_element_t c0 = handle_from_this(this_from_env(env)); (void)c0; hydw_log("fc->", s0, (r && r->type == V_OBJECT) ? (js_element_t)r->value : NULL); return r; }
static var_t* native_el_get_lastChild(vm_t* vm, var_t* env, void* data)         { return edge_child(vm, env, data, false, false); }
static var_t* native_el_get_firstElementChild(vm_t* vm, var_t* env, void* data) { return edge_child(vm, env, data, true,  true); }
static var_t* native_el_get_lastElementChild(vm_t* vm, var_t* env, void* data)  { return edge_child(vm, env, data, false, true); }

static var_t* native_el_get_nextSibling(vm_t* vm, var_t* env, void* data) {
    var_t* r = wrap_or_null(vm, sibling_of(state_any(vm, data), this_handle(env), 1, false));
    hydw_log("ns->", state_any(vm, data), (r && r->type == V_OBJECT) ? (js_element_t)r->value : NULL);
    return r;
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
    int appok = st->cb.el_append_child(st->ctx, el, ch);
    /* EWEB_IMGDBG: does the cover-image <img src=1200x/control1> actually get
     * appended into a laid-out parent, and does the append succeed? */
    {
        static int s_appdbg = -1;
        if(s_appdbg < 0) { const char* e = getenv("EWEB_IMGDBG"); s_appdbg = (e && e[0] == '1') ? 1 : 0; }
        if(s_appdbg && st->cb.el_get_attr != NULL) {
            char* cs = st->cb.el_get_attr(st->ctx, ch, "src");
            if(cs != NULL && (strstr(cs, "1200x") != NULL || strstr(cs, "control1") != NULL))
                fprintf(stderr, "[IMGDBG] coverimg append ok=%d child_el=%p parent_el=%p src=%.50s\n",
                    appok, (void*)ch, (void*)el, cs);
            if(cs != NULL) mario_free(cs);
        }
    }
    if(!appok) return var_new_null(vm);
    mo_notify_childlist(vm, st, el, ch, NULL);
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
        int apok = st->cb.el_append_child(st->ctx, el, ch);
        /* EWEB_IMGDBG: cover-image mount via the variadic append() path, which
         * bypasses native_el_appendChild (and thus its probe). */
        {
            static int s_app2dbg = -1;
            if(s_app2dbg < 0) { const char* e = getenv("EWEB_IMGDBG"); s_app2dbg = (e && e[0] == '1') ? 1 : 0; }
            if(s_app2dbg && st->cb.el_get_attr != NULL) {
                char* cs = st->cb.el_get_attr(st->ctx, ch, "src");
                if(cs != NULL && (strstr(cs, "1200x") != NULL || strstr(cs, "control1") != NULL))
                    fprintf(stderr, "[IMGDBG] coverimg append() ok=%d child_el=%p parent_el=%p src=%.50s\n",
                        apok, (void*)ch, (void*)el, cs);
                if(cs != NULL) mario_free(cs);
            }
        }
        mo_notify_childlist(vm, st, el, ch, NULL);
    }
    return var_new_null(vm);
}

/* ParentNode.prepend(...nodes): insert each before the current first child
 * (append when empty). github's adoptedStyleSheets/aria-notify polyfill calls
 * document.head.prepend(styleEl); without it the bundle threw and aborted. */
static var_t* native_el_prepend(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_insert_before == NULL)
        return var_new_null(vm);
    js_element_t ref = (st->cb.el_child_count != NULL && st->cb.el_child != NULL &&
                        st->cb.el_child_count(st->ctx, el) > 0)
                           ? st->cb.el_child(st->ctx, el, 0) : NULL;
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
        if(ref == NULL) st->cb.el_append_child(st->ctx, el, ch);
        else            st->cb.el_insert_before(st->ctx, el, ch, ref);
        mo_notify_childlist(vm, st, el, ch, NULL);
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
    int ibok = st->cb.el_insert_before(st->ctx, el, ch, ref);
    /* EWEB_IMGDBG: same cover-image trace on the insertBefore mount path. */
    {
        static int s_ibdbg = -1;
        if(s_ibdbg < 0) { const char* e = getenv("EWEB_IMGDBG"); s_ibdbg = (e && e[0] == '1') ? 1 : 0; }
        if(s_ibdbg && st->cb.el_get_attr != NULL) {
            char* cs = st->cb.el_get_attr(st->ctx, ch, "src");
            if(cs != NULL && (strstr(cs, "1200x") != NULL || strstr(cs, "control1") != NULL))
                fprintf(stderr, "[IMGDBG] coverimg insertBefore ok=%d child_el=%p parent_el=%p src=%.50s\n",
                    ibok, (void*)ch, (void*)el, cs);
            if(cs != NULL) mario_free(cs);
        }
    }
    if(!ibok) return var_new_null(vm);
    mo_notify_childlist(vm, st, el, ch, NULL);
    return (child != NULL) ? child : var_new_null(vm);
}

/* Env-gated (EWEB_MUTSTK) JS call-stack dump for unlink operations whose
 * parent is <body>: names the page-JS call chain that removed a top-level
 * node (e.g. rokid's bootstrap removing <main> before hydration). Frames
 * carry function-start pcs; map them onto scripts via the embedder's
 * pc_range table ("js: script N pc_range"). Read-only diagnostic. */
static void mutstk_dump(vm_t* vm, js_dom_state* st, const char* op,
                        js_element_t parent, js_element_t child) {
    static int on = -1;
    if(on < 0) on = (getenv("EWEB_MUTSTK") != NULL);
    if(!on || st == NULL || parent == NULL || st->cb.el_get_tag == NULL) return;
    char* pt = st->cb.el_get_tag(st->ctx, parent);
    if(pt == NULL || strcmp(pt, "body") != 0) { mario_free(pt); return; }
    char* ct = (child != NULL) ? st->cb.el_get_tag(st->ctx, child) : NULL;
    char* s = mario_format_stack(vm, NULL);
    fprintf(stderr, "[MUTSTK] %s parent=<%s> child=<%s>%s\n", op, pt,
            ct ? ct : "?", s ? s : "");
    mario_free(s);
    mario_free(ct);
    mario_free(pt);
}

static var_t* native_el_removeChild(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    var_t* child = js_arg(env, 0);
    js_element_t ch = element_arg(vm, child);
    if(st == NULL || el == NULL || ch == NULL || st->cb.el_remove_child == NULL)
        return var_new_null(vm);
    mutstk_dump(vm, st, "removeChild", el, ch);
    if(!st->cb.el_remove_child(st->ctx, el, ch)) return var_new_null(vm);
    mo_notify_childlist(vm, st, el, NULL, ch);
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
    mo_notify_childlist(vm, st, el, nn, old);
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
    if(p != NULL) {
        mutstk_dump(vm, st, "remove", p, el);
        st->cb.el_remove_child(st->ctx, p, el);
    }
    return NULL;
}

/* Coerce one replaceWith/before/after/replaceChildren argument to a node:
 * element wrappers pass through, strings become text nodes (DOM spec),
 * anything else is skipped. Returns NULL to skip. */
static js_element_t childnode_arg(vm_t* vm, js_dom_state* st, var_t* a) {
    js_element_t ch = element_arg(vm, a);
    if(ch == NULL && a != NULL && a->type == V_STRING &&
       st->cb.create_text_node != NULL) {
        mstr_t* tmp = mstr_new("");
        ch = st->cb.create_text_node(st->ctx, js_cstr(a, tmp));
        mstr_free(tmp);
    }
    return ch;
}

/* ChildNode.replaceWith(...nodes): swap `this` out of its parent for the given
 * nodes. React Router's link/scroll restoration and several polyfills call it
 * during commit; without it the bundle threw ("can not find function
 * 'replaceWith'") and the whole render aborted into the route error boundary.
 * Insert every node immediately before `this` (a constant ref keeps argument
 * order), then unlink `this`. Detached nodes no-op per spec. */
static var_t* native_el_replaceWith(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_parent == NULL ||
       st->cb.el_insert_before == NULL || st->cb.el_remove_child == NULL)
        return NULL;
    js_element_t p = st->cb.el_parent(st->ctx, el);
    if(p == NULL) return NULL;
    uint32_t n = get_func_args_num(env);
    for(uint32_t i = 0; i < n; i++) {
        js_element_t ch = childnode_arg(vm, st, js_arg(env, (int)i));
        if(ch == NULL || ch == el) continue;
        st->cb.el_insert_before(st->ctx, p, ch, el);
        mo_notify_childlist(vm, st, p, ch, NULL);
    }
    st->cb.el_remove_child(st->ctx, p, el);
    mo_notify_childlist(vm, st, p, NULL, el);
    return NULL;
}

/* ChildNode.before/after(...nodes): insert siblings adjacent to `this` in its
 * parent. `ref` is a constant anchor (this for before, this->nextSibling for
 * after) so successive inserts preserve argument order. */
static var_t* el_insert_siblings(vm_t* vm, var_t* env, void* data, int after) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_parent == NULL ||
       st->cb.el_insert_before == NULL || st->cb.el_append_child == NULL)
        return NULL;
    js_element_t p = st->cb.el_parent(st->ctx, el);
    if(p == NULL) return NULL;
    js_element_t ref = after ? sibling_of(st, el, 1, false) : el;
    uint32_t n = get_func_args_num(env);
    for(uint32_t i = 0; i < n; i++) {
        js_element_t ch = childnode_arg(vm, st, js_arg(env, (int)i));
        if(ch == NULL) continue;
        if(ref == NULL) st->cb.el_append_child(st->ctx, p, ch);
        else            st->cb.el_insert_before(st->ctx, p, ch, ref);
        mo_notify_childlist(vm, st, p, ch, NULL);
    }
    return NULL;
}
static var_t* native_el_before(vm_t* vm, var_t* env, void* data) {
    return el_insert_siblings(vm, env, data, 0);
}
static var_t* native_el_after(vm_t* vm, var_t* env, void* data) {
    return el_insert_siblings(vm, env, data, 1);
}

/* ParentNode.replaceChildren(...nodes): drop every child, then append the
 * arguments (empty call = clear). Used by sheet/portal teardown polyfills. */
static var_t* native_el_replaceChildren(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL || st->cb.el_remove_child == NULL)
        return NULL;
    while(st->cb.el_child_count != NULL && st->cb.el_child != NULL &&
          st->cb.el_child_count(st->ctx, el) > 0) {
        js_element_t c = st->cb.el_child(st->ctx, el, 0);
        if(c == NULL) break;
        if(!st->cb.el_remove_child(st->ctx, el, c)) break;
        mo_notify_childlist(vm, st, el, NULL, c);
    }
    return native_el_append(vm, env, data);
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

/* HTMLMediaElement stubs. rokid's IntersectionObserver-driven video autoplay
 * component runs `e.src||(e.src=a,e.load()), e.play().catch(()=>{})` and
 * `e.pause()`; without these the call misses ("can not find function
 * 'load'") and aborts the component's effect. There is no media pipeline, so
 * load()/pause() are no-ops and play() reports instant success. */
/* Defined further below (customElements section); reused as the no-Promise
 * fallback thenable for play(). */
static var_t* ce_then(vm_t* vm, var_t* env, void* data);
static var_t* ce_catch(vm_t* vm, var_t* env, void* data);

static var_t* native_el_media_noop(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;
}

/* Resolved-promise for play(). Prefer the real Promise.resolve(undefined) so
 * any chain (.catch/.then/await) behaves normally; fall back to the
 * immediately-settled thenable trick ce_then uses (same shape as
 * customElements.whenDefined's) when no Promise global is installed. */
static var_t* native_el_media_play(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    var_t* cls = var_find_member_var(vm->root, "Promise");
    var_t* res = (cls != NULL) ? var_find_member_var(cls, "resolve") : NULL;
    if(res != NULL && res->is_func) {
        var_t* args = var_new_array(vm);
        var_array_add(args, var_new_null(vm));
        var_t* p = call_m_func(vm, cls, res, args);
        var_unref(args);
        if(p != NULL) return p;
    }
    var_t* t = var_new_obj_no_proto(vm, NULL, NULL);
    vm_reg_static(vm, t, "then(fn)", ce_then, NULL);
    vm_reg_static(vm, t, "catch(fn)", ce_catch, NULL);
    return t;
}

/* Case-insensitive ASCII attribute-name compare (HTML attribute names are
 * ASCII and case-insensitive). Avoids strcasecmp's locale/include dependency. */
static int js_attr_name_cmp(const char* a, const char* b) {
    while(*a != 0 && *b != 0) {
        char ca = js_ascii_lower(*a), cb = js_ascii_lower(*b);
        if(ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Compare two el_attr_snapshot buffers ("name\x1fvalue\x1e...") as attribute
 * SETS per DOM isEqualNode (order-independent, with multiplicity). Both buffers
 * are mutable and tokenized in place; a matched B pair is consumed by emptying
 * its name (a real attribute name is never empty, so it cannot match twice).
 * Returns true when every A pair has a distinct B match and no B pair is left. */
static bool attr_snapshots_equal(char* a, char* b) {
    if(a == NULL || b == NULL) return (a == b);
    char* p = a;
    while(*p != 0) {
        char* sep = strchr(p, '\x1f');
        char* end = strchr(p, '\x1e');
        if(sep == NULL || end == NULL || sep > end) break;
        *sep = 0; *end = 0;
        const char* an = p;
        const char* av = sep + 1;
        bool matched = false;
        char* q = b;
        while(*q != 0) {
            char* s2 = strchr(q, '\x1f');
            char* e2 = strchr(q, '\x1e');
            if(s2 == NULL || e2 == NULL || s2 > e2) break;
            *s2 = 0; *e2 = 0;
            if(q[0] != 0 && js_attr_name_cmp(q, an) == 0 && strcmp(s2 + 1, av) == 0) {
                q[0] = '\0';       /* consume this B pair */
                matched = true;
                break;
            }
            q = e2 + 1;
        }
        if(!matched) return false;
        p = end + 1;
    }
    /* Any unconsumed B attribute means the counts/sets differ. */
    char* q = b;
    while(*q != 0) {
        char* s2 = strchr(q, '\x1f');
        char* e2 = strchr(q, '\x1e');
        if(s2 == NULL || e2 == NULL || s2 > e2) break;
        *s2 = 0; *e2 = 0;
        if(q[0] != 0) return false;
        q = e2 + 1;
    }
    return true;
}

/* Recursive structural equality of two node handles (DOM Node.isEqualNode).
 * Bounded depth so a corrupt tree cannot blow the stack. Uses the same cb
 * discriminators as native_el_get_nodeType/nodeValue/tagName. */
static bool el_node_equal(js_dom_state* st, js_element_t a, js_element_t b, int depth) {
    if(a == b) return true;
    if(a == NULL || b == NULL) return false;
    if(depth > 1024) return false;
    bool a_com = (st->cb.el_is_comment != NULL) && st->cb.el_is_comment(st->ctx, a);
    bool b_com = (st->cb.el_is_comment != NULL) && st->cb.el_is_comment(st->ctx, b);
    if(a_com != b_com) return false;
    bool a_tag = (st->cb.el_is_tag == NULL) ? true : st->cb.el_is_tag(st->ctx, a);
    bool b_tag = (st->cb.el_is_tag == NULL) ? true : st->cb.el_is_tag(st->ctx, b);
    if(a_tag != b_tag) return false;
    if(!a_tag) {
        /* Text / comment node: compare data. */
        if(st->cb.el_get_text == NULL) return true;
        char* ta = st->cb.el_get_text(st->ctx, a);
        char* tb = st->cb.el_get_text(st->ctx, b);
        bool eq = (ta == NULL || tb == NULL) ? (ta == tb) : (strcmp(ta, tb) == 0);
        mario_free(ta);
        mario_free(tb);
        return eq;
    }
    /* Element: tag name (case-insensitive; embedders fold case differently). */
    if(st->cb.el_get_tag != NULL) {
        char* na = st->cb.el_get_tag(st->ctx, a);
        char* nb = st->cb.el_get_tag(st->ctx, b);
        bool eq = (na == NULL || nb == NULL) ? (na == nb) : (js_attr_name_cmp(na, nb) == 0);
        mario_free(na);
        mario_free(nb);
        if(!eq) return false;
    }
    /* Attributes (set equality). */
    if(st->cb.el_attr_snapshot != NULL) {
        char* sa = st->cb.el_attr_snapshot(st->ctx, a);
        char* sb = st->cb.el_attr_snapshot(st->ctx, b);
        bool eq = attr_snapshots_equal(sa, sb);
        mario_free(sa);
        mario_free(sb);
        if(!eq) return false;
    }
    /* Children: same count, pairwise equal in document order. */
    if(st->cb.el_child_count != NULL && st->cb.el_child != NULL) {
        int na = st->cb.el_child_count(st->ctx, a);
        int nb = st->cb.el_child_count(st->ctx, b);
        if(na != nb) return false;
        for(int i = 0; i < na; ++i) {
            js_element_t ka = st->cb.el_child(st->ctx, a, i);
            js_element_t kb = st->cb.el_child(st->ctx, b, i);
            if(!el_node_equal(st, ka, kb, depth + 1)) return false;
        }
    }
    return true;
}

/* Node.isEqualNode(other): deep structural equality. Apple's global-header
 * virtual-DOM hydrator (see native_el_get_localName) and React reconciliation
 * call it to match server-rendered nodes against freshly built client nodes;
 * without it every comparison throws ("can not find function 'isEqualNode'")
 * and the hydrator aborts. A non-node / null argument compares false. */
static var_t* native_el_isEqualNode(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    js_element_t other = element_arg(vm, js_arg(env, 0));
    if(st == NULL || el == NULL || other == NULL) return var_new_bool(vm, false);
    return var_new_bool(vm, el_node_equal(st, el, other, 0));
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
    bool m;
    if(st != NULL && el != NULL && sel != NULL && sel[0] != 0 &&
       st->cb.el_matches != NULL) {
        /* Single-element callback: one select() on the element itself instead
         * of a full subtree query per matches() call. */
        m = st->cb.el_matches(st->ctx, el, sel);
    } else {
        js_element_t root = NULL;
        if(st != NULL && el != NULL && st->cb.el_parent != NULL)
            root = st->cb.el_parent(st->ctx, el);   /* NULL root => whole document */
        m = matches_selector(vm, st, el, root, sel);
    }
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

/* Current page scroll offset (0,0 when the embedder provides no hook). */
static void dom_scroll_off(js_dom_state* st, int* sx, int* sy) {
    int x = 0, y = 0;
    if(st != NULL && st->cb.get_scroll != NULL) st->cb.get_scroll(st->ctx, &x, &y);
    if(sx != NULL) *sx = x;
    if(sy != NULL) *sy = y;
}

/* position:fixed boxes are pinned to the viewport by layout, so their document
 * coordinates ARE already viewport coordinates: the scroll shift must not be
 * applied to them or a fixed header would drift off-screen as the page scrolls. */
static bool el_is_fixed(js_dom_state* st, js_element_t el) {
    if(st == NULL || el == NULL || st->cb.el_get_style == NULL) return false;
    char* p = st->cb.el_get_style(st->ctx, el, "position");
    if(p == NULL) return false;
    bool fixed = (strcmp(p, "fixed") == 0);
    mario_free(p);
    return fixed;
}

/* el_rect shifted into viewport coordinates (what getBoundingClientRect and
 * IntersectionObserver report). The document box is first rewritten into its
 * CSS-transformed bounds (gBCR semantics; the offset* properties keep the
 * untransformed layout box el_rect reports), then shifted; fixed elements are
 * left untouched by the shift. */
static bool el_viewport_rect(js_dom_state* st, js_element_t el,
                             int* x, int* y, int* w, int* h) {
    int lx = 0, ly = 0, lw = 0, lh = 0;
    if(!el_rect(st, el, &lx, &ly, &lw, &lh)) return false;
    if(st->cb.el_transform_rect != NULL)
        st->cb.el_transform_rect(st->ctx, el, &lx, &ly, &lw, &lh);
    if(!el_is_fixed(st, el)) {
        int sx = 0, sy = 0;
        dom_scroll_off(st, &sx, &sy);
        lx -= sx;
        ly -= sy;
    }
    if(x != NULL) *x = lx;
    if(y != NULL) *y = ly;
    if(w != NULL) *w = lw;
    if(h != NULL) *h = lh;
    return true;
}

static var_t* native_el_getBoundingClientRect(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    int x = 0, y = 0, w = 0, h = 0;
    el_viewport_rect(st, el, &x, &y, &w, &h);
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
    js_element_t el = this_handle(env);
    int x = 0, y = 0, w = 0, h = 0;
    el_rect(st, el, &x, &y, &w, &h);
    switch(which) {
        case 0:  return var_new_int(vm, w);
        case 1:  return var_new_int(vm, h);
        case 2: case 3: {
            /* offsetLeft/Top are relative to the OFFSET PARENT's border box
             * (nearest positioned ancestor, else body), not the document: a
             * child of an absolutely positioned card must read its offset
             * inside the card. The embedder reports document coordinates, so
             * the offsetParent origin is subtracted here. */
            if(st != NULL && st->cb.el_offset_parent_origin != NULL) {
                int ox = 0, oy = 0;
                st->cb.el_offset_parent_origin(st->ctx, el, &ox, &oy);
                x -= ox;
                y -= oy;
            }
            return var_new_int(vm, which == 2 ? x : y);
        }
        case 4: case 5: {
            /* clientWidth/Height of the scrolling element (html/body) is the
             * VIEWPORT, not the (usually much taller) document box: pages test
             * scrollHeight>clientHeight to decide whether scrolling is even
             * possible, so reporting the document height here hides the scroll. */
            if(st != NULL && st->cb.get_viewport != NULL &&
               (el == doc_node(st, 0) || el == doc_node(st, 2))) {
                int vw = 0, vh = 0;
                st->cb.get_viewport(st->ctx, &vw, &vh);
                return var_new_int(vm, (which == 4) ? vw : vh);
            }
            /* client* is the padding box, el_rect the border box: ask the
             * embedder for the former when it can report it. */
            if(st != NULL && st->cb.el_get_client_size != NULL) {
                int cw = 0, ch = 0;
                st->cb.el_get_client_size(st->ctx, el, &cw, &ch);
                return var_new_int(vm, (which == 4) ? cw : ch);
            }
            return var_new_int(vm, (which == 4) ? w : h);
        }
        case 6: case 7: {
            /* scrollWidth/Height of the scrolling element (html/body) is the
             * DOCUMENT content size, not the viewport box el_get_rect reports
             * for it: pages compare scrollHeight against clientHeight/scrollY
             * to drive lazy loading and end-of-page detection, so reporting
             * the viewport here makes them read "cannot scroll / at bottom". */
            if(st != NULL && st->cb.get_document_size != NULL &&
               (el == doc_node(st, 0) || el == doc_node(st, 2))) {
                int dw = 0, dh = 0;
                st->cb.get_document_size(st->ctx, &dw, &dh);
                return var_new_int(vm, (which == 6) ? dw : dh);
            }
            return var_new_int(vm, (which == 6) ? w : h);
        }
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

/* Document child traversal. The Document singleton carries no element handle
 * (this_handle() is NULL for it), so the generic Element/Node child walk it
 * inherits returns an empty list - but React's hydrateRoot(document) locates
 * the host tree to hydrate by walking document.firstChild / childNodes, and an
 * empty list makes it hydrate nothing: a silent empty commit with no fibers on
 * any SSR node (rokid's client header never mounts). Expose the document
 * element as the document's sole child so the walk sees the real tree. */
static var_t* native_document_get_firstChild(vm_t* vm, var_t* env, void* data) {
    (void)env; return doc_node_get(vm, data, 0);
}
static var_t* native_document_get_lastChild(vm_t* vm, var_t* env, void* data) {
    (void)env; return doc_node_get(vm, data, 0);
}
static var_t* native_document_get_firstElementChild(vm_t* vm, var_t* env, void* data) {
    (void)env; return doc_node_get(vm, data, 0);
}
static var_t* native_document_get_lastElementChild(vm_t* vm, var_t* env, void* data) {
    (void)env; return doc_node_get(vm, data, 0);
}
static var_t* native_document_get_childNodes(vm_t* vm, var_t* env, void* data) {
    (void)env;
    js_dom_state* st = state_any(vm, data);
    var_t* arr = var_new_array(vm);
    js_element_t de = doc_node(st, 0);
    if(de != NULL) {
        vm->gc.gc_defer++;
        arr_add_element(vm, arr, de);
        vm->gc.gc_defer--;
    }
    return arr;
}
static var_t* native_document_get_children(vm_t* vm, var_t* env, void* data) {
    return native_document_get_childNodes(vm, env, data);
}
static var_t* native_document_get_childElementCount(vm_t* vm, var_t* env, void* data) {
    (void)env;
    return var_new_int(vm, (doc_node(state_any(vm, data), 0) != NULL) ? 1 : 0);
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

/* document.elementFromPoint(x,y): the topmost element under a viewport point.
 * The embedder's hit walker already resolves paint order and scroll, so this
 * is a thin pass-through; null when the point is outside the page. */
static var_t* native_document_elementFromPoint(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    if(st == NULL || st->cb.el_from_point == NULL) return var_new_null(vm);
    js_element_t el = st->cb.el_from_point(st->ctx, js_arg_int(env, 0), js_arg_int(env, 1));
    if(el == NULL) return var_new_null(vm);
    return wrap_or_null(vm, el);
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
    (void)env;
    /* Scripts run during the build phase, BEFORE jsFireLoadEvents dispatches
     * DOMContentLoaded/load. Reporting "complete" that early is a lie: pages
     * gate lifecycle work on it. Next.js's flight bootstrap does
     *   "loading"===readyState ? addEventListener("DOMContentLoaded",j) : j();
     * and j() closes the ReadableStream feeding the RSC parser. With a constant
     * "complete", j() runs before the stream's controller exists, so the stream
     * is never closed, React's reader parks forever on read(), the root render
     * suspends and nothing commits (the top nav never mounts). Report "loading"
     * until DOMContentLoaded actually fires, then "complete", so j() defers to
     * the event we dispatch at the end of the build - matching a real browser.
     *
     * Exception: a real browser runs defer/module scripts AFTER parsing ends,
     * so they observe readyState=="interactive". Github's client-env module
     * gates reading its JSON island on that value; with "loading" it defers to
     * DOMContentLoaded while module-level consumers call it immediately and
     * throw. The embedder flips parsing_complete around those scripts. */
    js_dom_state* st = state_any(vm, data);
    if(st != NULL && !st->dom_loaded) {
        if(st->parsing_complete || st->in_async_cb)
            return var_new_str(vm, "interactive");
        return var_new_str(vm, "loading");
    }
    return var_new_str(vm, "complete");
}

/* Flip the document to the loaded phase. Called by js_event right before
 * DOMContentLoaded is dispatched so any handler that reads readyState (and the
 * flight bootstrap's deferred j()) sees the post-load value. */
void js_dom_mark_dom_loaded(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    if(st != NULL) st->dom_loaded = true;
}

/* Tell the bridge whether the parser has finished, so readyState reports
 * "interactive" while deferred/module scripts run (see the getter above).
 * The embedder brackets each such script with true/false. */
void js_dom_set_parsing_complete(vm_t* vm, bool complete) {
    js_dom_state* st = state_from_vm(vm);
    if(st != NULL) st->parsing_complete = complete;
}

/* Mark whether an ASYNC event-loop callback is running so readyState reports
 * "interactive" for it (see in_async_cb / the getter). The embedder brackets
 * every jsVmEnter(appScript=false) run - timer poll, MessageChannel drain,
 * fetch settle, event dispatch - with true, and clears it on exit. */
void js_dom_set_async_cb(vm_t* vm, bool async) {
    js_dom_state* st = state_from_vm(vm);
    if(st != NULL) st->in_async_cb = async;
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
    return var_new_int(vm, js_add_timer(vm, st, cb, JS_FRAME_MS, false, false, false));
}

static var_t* native_cancelAnimationFrame(vm_t* vm, var_t* env, void* data) {
    return native_clearTimeout(vm, env, data);
}

/* ------------------------------------------------------------------ */
/* DOM observers (Mutation / Intersection / Resize)                    */
/*                                                                     */
/* Bundles construct these and immediately call observe(); with the    */
/* classes left empty that call throws "can not find function" and     */
/* aborts the whole boot chain. MutationObserver additionally DELIVERS */
/* its callback asynchronously when an observed node's characterData   */
/* (.data / .nodeValue) or an observed attribute is written: core-js's */
/* queueMicrotask polyfill re-arms through exactly this - a text node  */
/* whose .data it toggles - so without delivery every microtask, and   */
/* thus every React Server-Component flight reaction, is stranded and  */
/* the RSC stream stalls after its first chunk. Delivery rides the DOM */
/* timer table at 0 ms like MessageChannel/queueMicrotask, coalesced   */
/* to one callback per observer per turn. IntersectionObserver delivers */
/* a one-shot intersecting entry per observed target (see native_io_*); */
/* ResizeObserver delivers an initial laid-out size after observe().      */
/* ------------------------------------------------------------------ */
#define OB_REG    "@@obreg"    /* root hidden array of live MutationObservers */
#define OB_CB     "@@obcb"     /* observer -> callback function */
#define OB_TG     "@@obtg"     /* observer -> array of observed target wrappers */
#define OB_OPT    "@@obopt"    /* observer -> array of options, parallel to OB_TG */
#define OB_PEND   "@@obpend"   /* observer -> pending records (coalescing buffer) */
#define OB_TIMER  "@@obtimer"  /* observer -> armed 0 ms dispatch timer id (0 = none) */
#define OB_DEAD   "@@obdead"   /* observer -> disconnected flag */

static void ob_hide(node_t* n) { if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; } }

static var_t* ob_reg(vm_t* vm) {
    var_t* arr = var_find_own_member_var(vm->root, OB_REG);
    if(arr == NULL) {
        ob_hide(var_add(vm->root, OB_REG, var_new_array(vm)));
        arr = var_find_own_member_var(vm->root, OB_REG);
    }
    return arr;
}

static var_t* ob_arr(vm_t* vm, var_t* observer, const char* key) {
    var_t* arr = var_find_own_member_var(observer, key);
    if(arr == NULL) {
        ob_hide(var_add(observer, key, var_new_array(vm)));
        arr = var_find_own_member_var(observer, key);
    }
    return arr;
}

/* Rebuild @@obreg without `observer` (mirrors mc_unanchor). */
static void mo_unanchor(vm_t* vm, var_t* observer) {
    var_t* reg = var_find_own_member_var(vm->root, OB_REG);
    if(reg == NULL) return;
    vm->gc.gc_defer++;
    var_t* fresh = var_new_array(vm);
    uint32_t sz = var_array_size(reg);
    for(uint32_t i = 0; i < sz; ++i) {
        node_t* nd = var_array_get(reg, (int32_t)i);
        if(nd != NULL && nd->var != NULL && nd->var != observer)
            var_array_add(fresh, nd->var);
    }
    ob_hide(var_add(vm->root, OB_REG, fresh));
    vm->gc.gc_defer--;
}

typedef struct mo_disp { var_t* observer; } mo_disp_t;

/* 0 ms timer trampoline: deliver the coalesced records to the observer's cb. */
static var_t* native_mo_dispatch(vm_t* vm, var_t* env, void* data) {
    (void)env;
    mo_disp_t* d = (mo_disp_t*)data;
    if(d == NULL) return NULL;
    var_t* observer = d->observer;
    mario_free(d);
    if(observer == NULL || observer->status <= V_ST_GC_FREE) return NULL;
    var_add(observer, OB_TIMER, var_new_int(vm, 0));   /* no longer armed */
    var_t* pend = var_find_own_member_var(observer, OB_PEND);
    var_t* records = (pend != NULL) ? pend : var_new_array(vm);
    /* Swap in a fresh buffer BEFORE the callback so a write the callback itself
     * causes schedules a clean next delivery instead of appending to the list
     * we are about to hand out. */
    ob_hide(var_add(observer, OB_PEND, var_new_array(vm)));
    var_t* dead = var_find_own_member_var(observer, OB_DEAD);
    int is_dead = (dead != NULL && var_get_int(dead) != 0);
    if(getenv("MARIO_MODBG") != NULL) {
        fprintf(stderr, "[modbg] dispatch observer=%p records=%p records=%u dead=%d rectype=%d\n",
            (void*)observer, (void*)records, (unsigned)var_array_size(records), is_dead,
            (int)records->type);
        for(uint32_t ri = 0; ri < var_array_size(records) && ri < 4; ++ri) {
            var_t* rc = var_array_get_var(records, (int32_t)ri);
            var_t* an = (rc != NULL) ? var_find_member_var(rc, "addedNodes") : NULL;
            fprintf(stderr, "[modbg]   rec#%u rc=%p rtype=%d addedNodes=%p antype=%d anisarr=%d\n",
                (unsigned)ri, (void*)rc, (rc != NULL) ? (int)rc->type : -1,
                (void*)an, (an != NULL) ? (int)an->type : -1,
                (an != NULL) ? (int)an->is_array : -1);
        }
    }
    if(!is_dead) {
        var_t* cb = var_find_own_member_var(observer, OB_CB);
        if(cb != NULL && cb->is_func) {
            var_t* args = var_new_array(vm);
            var_array_add(args, records);
            var_array_add(args, observer);
            var_array_reverse(args);   /* call_m_func wants the last arg at index 0 */
            var_t* r = call_m_func(vm, observer, cb, args);
            if(r != NULL) var_unref(r);
            var_unref(args);
        }
    }
    /* A disconnect() that arrived while this dispatch was armed deferred the
     * unanchor to here (it kept the observer rooted so d->observer stayed
     * valid); drop it from the registry now that we are done with it. */
    if(is_dead) mo_unanchor(vm, observer);
    var_unref(records);   /* release mo_schedule's arm-time ownership ref */
    return NULL;
}

/* Append a record and ensure exactly one 0 ms dispatch is armed for observer. */
static void mo_schedule(vm_t* vm, var_t* observer, var_t* record) {
    var_t* pend = ob_arr(vm, observer, OB_PEND);
    if(pend != NULL && record != NULL) var_array_add(pend, record);
    /* Hold an extra ownership ref on the coalescing buffer until the dispatch
     * trampoline consumes it. The buffer is otherwise owned only by the
     * observer's hidden @@obpend slot; a var_add() that re-seats that slot (the
     * dispatch's swap-in of a fresh buffer) or a refcount drop elsewhere freed
     * it while records were still pending, so the callback received a recycled
     * var (type 0) and `for (const r of records)` threw "object is not
     * iterable", stranding catalyst/react-partial element upgrades. Taken once
     * per armed dispatch cycle (not per record) so exactly one ref is held per
     * trampoline; the matching release is in native_mo_dispatch after the
     * callback ran (and on the arm-failure path below). */
    if(getenv("MARIO_MODBG") != NULL)
        fprintf(stderr, "[modbg] schedule observer=%p pend=%p ptype=%d psize=%u rec=%p\n",
            (void*)observer, (void*)pend, (pend != NULL) ? (int)pend->type : -1,
            (pend != NULL) ? (unsigned)var_array_size(pend) : 0u, (void*)record);
    var_t* tid = var_find_own_member_var(observer, OB_TIMER);
    if(tid != NULL && var_get_int(tid) != 0) return;   /* already armed: coalesce */
    if(pend != NULL) var_ref(pend);   /* keep the buffer alive until dispatch */
    mo_disp_t* d = (mo_disp_t*)mario_malloc(sizeof(mo_disp_t));
    if(d == NULL) { if(pend != NULL) var_unref(pend); return; }
    d->observer = observer;
    var_t* tr = var_new_native_func(vm, native_mo_dispatch, d);
    /* MutationObserver callbacks are microtasks per the DOM spec: they run at the
     * microtask checkpoint, after the current task and before the next macrotask.
     * Several polyfills (core-js included) rely on MO as a microtask drain, so it
     * must ride the microtask priority, not the 0-ms macrotask slot. */
    int id = js_dom_add_microtask(vm, tr);
    if(getenv("MARIO_MODBG") != NULL)
        fprintf(stderr, "[modbg] schedule observer=%p timer=%d\n", (void*)observer, id);
    if(id == 0) { mario_free(d); var_unref(tr); if(pend != NULL) var_unref(pend); return; }  /* table full: retried next write */
    var_add(observer, OB_TIMER, var_new_int(vm, id));
    /* tr is owned solely by the timer table's @@timers anchor (var_new_* starts
     * at refs==0): do NOT unref it here or it is freed before it fires (the
     * exact MessageChannel trap). d->observer stays valid because the observer
     * is rooted in @@obreg from observe() until dispatch/disconnect. */
}

/* Fire observers watching `el`. is_cd: 1 = characterData write, 0 = attribute. */
static void mo_notify_charattr(vm_t* vm, js_element_t el, int is_cd, const char* attr) {
    if(vm == NULL || el == NULL) return;
    var_t* reg = var_find_own_member_var(vm->root, OB_REG);
    if(reg == NULL) return;   /* no observers: the overwhelmingly common case */
    uint32_t sz = var_array_size(reg);
    for(uint32_t i = 0; i < sz; ++i) {
        node_t* nd = var_array_get(reg, (int32_t)i);
        if(nd == NULL || nd->var == NULL) continue;
        var_t* observer = nd->var;
        var_t* dead = var_find_own_member_var(observer, OB_DEAD);
        if(dead != NULL && var_get_int(dead) != 0) continue;
        var_t* tg = var_find_own_member_var(observer, OB_TG);
        var_t* op = var_find_own_member_var(observer, OB_OPT);
        if(tg == NULL) continue;
        uint32_t tn = var_array_size(tg);
        for(uint32_t j = 0; j < tn; ++j) {
            node_t* tnd = var_array_get(tg, (int32_t)j);
            if(tnd == NULL || tnd->var == NULL) continue;
            node_t* ond = (op != NULL) ? var_array_get(op, (int32_t)j) : NULL;
            var_t* opts = (ond != NULL) ? ond->var : NULL;
            const char* key = is_cd ? "characterData" : "attributes";
            var_t* flag = (opts != NULL) ? var_find_member_var(opts, key) : NULL;
            if(flag == NULL || !var_get_bool(flag)) continue;
            js_element_t target = handle_from_this(tnd->var);
            if(target == NULL) {
                /* Document-object observer (new_obj, not wrap_element): matches
                 * any node's attribute / characterData change when subtree:true,
                 * since Document is the root ancestor of every node. Mirrors the
                 * mo_notify_childlist document-target handling. */
                var_t* docv = var_find_member_var(vm->root, "document");
                if(docv == NULL || docv != tnd->var) continue;
                var_t* dsub = (opts != NULL) ? var_find_member_var(opts, "subtree") : NULL;
                if(dsub == NULL || !var_get_bool(dsub)) continue;
            } else if(target != el) {
                continue;
            }
            var_t* rec = var_new_obj_no_proto(vm, NULL, NULL);
            var_add(rec, "type", var_new_str(vm, is_cd ? "characterData" : "attributes"));
            var_add(rec, "target", tnd->var);
            var_add(rec, "addedNodes", var_new_array(vm));
            var_add(rec, "removedNodes", var_new_array(vm));
            var_add(rec, "previousSibling", var_new_null(vm));
            var_add(rec, "nextSibling", var_new_null(vm));
            var_add(rec, "attributeName", is_cd ? var_new_null(vm) : var_new_str(vm, attr != NULL ? attr : ""));
            var_add(rec, "attributeNamespace", var_new_null(vm));
            var_add(rec, "oldValue", var_new_null(vm));
            if(getenv("MARIO_MODBG") != NULL)
                fprintf(stderr, "[modbg] notify observer=%p target=%p %s\n",
                    (void*)observer, (void*)tnd->var, is_cd ? "characterData" : "attributes");
            mo_schedule(vm, observer, rec);
            break;   /* one record per observer per write */
        }
    }
}

/* Fire observers watching `parent` for a childList change. An observer matches
 * when its target IS `parent`, or - if it asked for subtree:true - when its
 * target is an ancestor of `parent` (walked via the embedder's el_parent).
 * Without this, every DOM-mutation watcher stays silent: polyfills and boot
 * code that wait for a node to appear (React's root attach, relay data-island
 * injection, core-js feature probes) park forever on a promise that only a
 * childList record would settle. */
/* Record `added`'s tag for a later upgrade pass iff it names a REGISTERED
 * custom element that is not already queued. Placed at the very top of
 * mo_notify_childlist (which every insertion native funnels through) so it runs
 * even when no MutationObserver is registered - the common case, where
 * mo_notify_childlist otherwise returns early. Cheap: two hash lookups and, for
 * the overwhelmingly common non-custom-element child, an immediate miss. */
static void ce_mark_dirty(vm_t* vm, js_dom_state* st, js_element_t added) {
    /* Kill-switch for A/B attribution of hydration-time faults (cached; one
     * predictable branch per insertion when off). */
    static int s_cedirty_off = -1;
    if(s_cedirty_off < 0) s_cedirty_off = (getenv("EWEB_CEDIRTY_OFF") != NULL);
    if(s_cedirty_off) return;
    if(st == NULL || added == NULL || st->cb.el_get_tag == NULL) return;
    var_t* reg = var_find_member_var(vm->root, "@@customelements");
    if(reg == NULL) return;   /* no custom elements defined yet */
    char* tag = st->cb.el_get_tag(st->ctx, added);
    if(tag == NULL) return;
    char buf[CE_DIRTY_NAME];
    int k = 0;
    for(char* p = tag; *p != 0 && k < CE_DIRTY_NAME - 1; ++p)
        buf[k++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
    buf[k] = 0;
    /* el_get_tag strdup's via mario_malloc (see jsElGetTag); we only need the
     * lowercased copy in buf, and this runs on every insertion, so release it
     * here or each appendChild leaks a string. */
    mario_free(tag);
    if(k == 0) return;
    if(var_find_own_member(reg, buf) == NULL) return;   /* not a custom element */
    for(int i = 0; i < st->ce_dirty_n; ++i)
        if(strcmp(st->ce_dirty[i], buf) == 0) return;   /* already queued */
    if(st->ce_dirty_n >= CE_DIRTY_MAX) return;
    strcpy(st->ce_dirty[st->ce_dirty_n++], buf);
}

/* Walk a freshly attached subtree (innerHTML / fragment parse) and mark every
 * registered custom element for the insertion-upgrade drain. The port attaches
 * parsed fragments without going through mo_notify_childlist, so without this
 * walk an innerHTML-created <relative-time> would never run connectedCallback.
 * Depth- and child-bounded so a pathological fragment cannot stall the tick;
 * ce_mark_dirty itself is a cheap double hash lookup plus a strdup/free. */
static void ce_mark_subtree_dirty(vm_t* vm, js_dom_state* st, js_element_t el, int depth) {
    if(el == NULL || depth > 24) return;
    if(st->cb.el_child_count == NULL || st->cb.el_child == NULL) return;
    int n = st->cb.el_child_count(st->ctx, el);
    if(n < 0 || n > 4096) return;
    for(int i = 0; i < n; ++i) {
        js_element_t ch = st->cb.el_child(st->ctx, el, i);
        if(ch == NULL) continue;
        ce_mark_dirty(vm, st, ch);
        ce_mark_subtree_dirty(vm, st, ch, depth + 1);
    }
}

static void mo_notify_childlist(vm_t* vm, js_dom_state* st, js_element_t parent,
                                js_element_t added, js_element_t removed) {
    if(vm == NULL || parent == NULL) return;
    /* Custom-element insertion reaction (before the no-observer early return
     * below): a late-inserted registered element needs an upgrade pass. */
    if(added != NULL) ce_mark_dirty(vm, st, added);
    var_t* reg = var_find_own_member_var(vm->root, OB_REG);
    if(reg == NULL) return;   /* no observers: the overwhelmingly common case */
    uint32_t sz = var_array_size(reg);
    if(sz == 0) return;
    for(uint32_t i = 0; i < sz; ++i) {
        node_t* nd = var_array_get(reg, (int32_t)i);
        if(nd == NULL || nd->var == NULL) continue;
        var_t* observer = nd->var;
        var_t* dead = var_find_own_member_var(observer, OB_DEAD);
        if(dead != NULL && var_get_int(dead) != 0) continue;
        var_t* tg = var_find_own_member_var(observer, OB_TG);
        var_t* op = var_find_own_member_var(observer, OB_OPT);
        if(tg == NULL) continue;
        uint32_t tn = var_array_size(tg);
        for(uint32_t j = 0; j < tn; ++j) {
            node_t* tnd = var_array_get(tg, (int32_t)j);
            if(tnd == NULL || tnd->var == NULL) continue;
            node_t* ond = (op != NULL) ? var_array_get(op, (int32_t)j) : NULL;
            var_t* opts = (ond != NULL) ? ond->var : NULL;
            var_t* flag = (opts != NULL) ? var_find_member_var(opts, "childList") : NULL;
            if(flag == NULL || !var_get_bool(flag)) continue;
            js_element_t target = handle_from_this(tnd->var);
            bool doc_target = false;
            if(target == NULL) {
                /* The observer may target the Document OBJECT rather than an
                 * element wrapper. Document is created with new_obj() (not
                 * wrap_element()), so handle_from_this() rejects it and the old
                 * `target == NULL -> continue` silently dropped every document
                 * observer. Document is the root ancestor of every element, so
                 * with subtree:true it must match a childList mutation anywhere
                 * in the tree. React's streaming-reveal helper observes
                 * `document` exactly this way to learn when a streamed node
                 * (e.g. #__PWS_INITIAL_PROPS__) appears; without this the
                 * observer never fired and hydration stalled. */
                var_t* docv = var_find_member_var(vm->root, "document");
                if(docv == NULL || docv != tnd->var) continue;
                var_t* dsub = (opts != NULL) ? var_find_member_var(opts, "subtree") : NULL;
                if(dsub == NULL || !var_get_bool(dsub)) continue;
                doc_target = true;
            }
            if(!doc_target && target != parent) {
                var_t* sub = (opts != NULL) ? var_find_member_var(opts, "subtree") : NULL;
                if(sub == NULL || !var_get_bool(sub)) continue;
                if(st == NULL || st->cb.el_parent == NULL) continue;
                js_element_t cur = st->cb.el_parent(st->ctx, parent);
                while(cur != NULL && cur != target)
                    cur = st->cb.el_parent(st->ctx, cur);
                if(cur != target) continue;
            }
            vm->gc.gc_defer++;
            var_t* rec = var_new_obj_no_proto(vm, NULL, NULL);
            var_add(rec, "type", var_new_str(vm, "childList"));
            var_add(rec, "target", tnd->var);
            var_t* an = var_new_array(vm);
            if(added != NULL)   var_array_add(an, wrap_element(vm, added));
            var_add(rec, "addedNodes", an);
            var_t* rn = var_new_array(vm);
            if(removed != NULL) var_array_add(rn, wrap_element(vm, removed));
            var_add(rec, "removedNodes", rn);
            var_add(rec, "previousSibling", var_new_null(vm));
            var_add(rec, "nextSibling", var_new_null(vm));
            var_add(rec, "attributeName", var_new_null(vm));
            var_add(rec, "attributeNamespace", var_new_null(vm));
            var_add(rec, "oldValue", var_new_null(vm));
            vm->gc.gc_defer--;
            if(getenv("MARIO_MODBG") != NULL)
                fprintf(stderr, "[modbg] notify childList observer=%p target=%p\n",
                    (void*)observer, (void*)tnd->var);
            mo_schedule(vm, observer, rec);
            break;   /* one record per observer per mutation */
        }
    }
}

static var_t* native_observer_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = get_obj(env, THIS);
    if(self == NULL) return NULL;
    /* Keep the callback alive on the instance so delivery/takeRecords can reach
     * it; hidden name keeps it off JS eyes. */
    var_t* cb = js_arg_func(env, 0);
    var_add(self, OB_CB, cb != NULL ? cb : var_new(vm));
    return NULL;
}

static var_t* native_observer_observe(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = get_obj(env, THIS);
    if(self == NULL) return NULL;
    var_t* args = get_func_args(env);
    node_t* tn = var_array_get(args, 0);
    node_t* on = var_array_get(args, 1);
    var_t* target = (tn != NULL) ? tn->var : NULL;
    var_t* options = (on != NULL) ? on->var : NULL;
    if(target == NULL) return NULL;
    var_add(self, OB_DEAD, var_new_int(vm, 0));   /* (re)observing clears a prior disconnect */
    var_t* tg = ob_arr(vm, self, OB_TG);
    var_t* op = ob_arr(vm, self, OB_OPT);
    if(tg != NULL) var_array_add(tg, target);
    if(op != NULL) var_array_add(op, options != NULL ? options : var_new(vm));
    var_t* reg = ob_reg(vm);
    if(reg != NULL) {
        uint32_t sz = var_array_size(reg);
        bool present = false;
        for(uint32_t i = 0; i < sz; ++i) {
            node_t* nd = var_array_get(reg, (int32_t)i);
            if(nd != NULL && nd->var == self) { present = true; break; }
        }
        if(!present) { vm->gc.gc_defer++; var_array_add(reg, self); vm->gc.gc_defer--; }
    }
    if(getenv("MARIO_MODBG") != NULL)
        fprintf(stderr, "[modbg] observe self=%p target=%p opts=%p\n",
            (void*)self, (void*)target, (void*)options);
    return NULL;   /* undefined */
}

/* IntersectionObserver delivery (ewebview): the engine has no scroll/viewport
 * event feed, yet many sites gate visibility on an IO callback firing with an
 * intersecting entry - apple.com's "StaggeredFadeInTween" keeps a section at
 * opacity:0 (class enhanced--initial) until the callback reads
 * entry.isIntersecting truthy and swaps in the visible class. With no delivery
 * the reveal never runs and whole sections (the Mac product lineup gallery)
 * stay invisible. A static full-page render is best approximated by reporting
 * every observed target as fully intersecting once, shortly after observe(),
 * so the reveal/enhance JS runs. Delivered on the DOM timer table as a small
 * macrotask so the observing script turn finishes first. MutationObserver and
 * ResizeObserver independently queues a delayed initial size delivery. */
typedef struct io_disp { var_t* observer; var_t* target; } io_disp_t;

static var_t* native_io_dispatch(vm_t* vm, var_t* env, void* data) {
    (void)env;
    io_disp_t* d = (io_disp_t*)data;
    if(d == NULL) return NULL;
    var_t* observer = d->observer;
    var_t* target   = d->target;
    mario_free(d);
    if(observer == NULL || observer->status <= V_ST_GC_FREE) return NULL;
    var_t* dead = var_find_own_member_var(observer, OB_DEAD);
    if(dead != NULL && var_get_int(dead) != 0) return NULL;
    var_t* cb = var_find_own_member_var(observer, OB_CB);
    if(cb == NULL || !cb->is_func) return NULL;
    if(target == NULL || target->status <= V_ST_GC_FREE) return NULL;
    /* one fully-intersecting entry; rects are zero-filled (reveal callbacks read
     * only isIntersecting, but destructuring must not trip on missing fields) */
    var_t* rect = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(rect, "x", var_new_int(vm, 0));
    var_add(rect, "y", var_new_int(vm, 0));
    var_add(rect, "width", var_new_int(vm, 0));
    var_add(rect, "height", var_new_int(vm, 0));
    var_add(rect, "top", var_new_int(vm, 0));
    var_add(rect, "right", var_new_int(vm, 0));
    var_add(rect, "bottom", var_new_int(vm, 0));
    var_add(rect, "left", var_new_int(vm, 0));
    var_t* entry = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(entry, "isIntersecting", var_new_bool(vm, true));
    var_add(entry, "intersectionRatio", var_new_float64(vm, 1.0));
    var_add(entry, "target", target);
    var_add(entry, "boundingClientRect", rect);
    var_add(entry, "intersectionRect", rect);
    var_add(entry, "rootBounds", var_new_null(vm));
    var_add(entry, "time", var_new_float64(vm, 0.0));
    var_t* entries = var_new_array(vm);
    var_array_add(entries, entry);
    var_t* args = var_new_array(vm);
    var_array_add(args, entries);
    var_array_add(args, observer);
    var_array_reverse(args);   /* call_m_func wants the last arg at index 0 */
    var_t* r = call_m_func(vm, observer, cb, args);
    if(r != NULL) var_unref(r);
    var_unref(args);
    return NULL;
}

static void io_schedule(vm_t* vm, void* bridge, var_t* observer, var_t* target) {
    js_dom_state* st = state_any(vm, bridge);
    if(st == NULL || observer == NULL || target == NULL) return;
    io_disp_t* d = (io_disp_t*)mario_malloc(sizeof(io_disp_t));
    if(d == NULL) return;
    d->observer = observer;
    d->target   = target;
    var_t* tr = var_new_native_func(vm, native_io_dispatch, d);
    int id = js_add_timer(vm, st, tr, 48, false, false, false);
    if(id == 0) { mario_free(d); var_unref(tr); return; }
    /* tr is owned by the timer table anchor (starts at refs==0): do NOT unref
     * here or it is freed before it fires. observer stays rooted in @@obreg and
     * target in the observer's OB_TG array, so both survive the delay. */
}

/* IntersectionObserver.observe(): register the target like the base observer,
 * then queue a one-shot intersecting delivery for it (see native_io_dispatch). */
static var_t* native_io_observe(vm_t* vm, var_t* env, void* data) {
    var_t* r = native_observer_observe(vm, env, data);
    if(r != NULL) var_unref(r);
    var_t* self = get_obj(env, THIS);
    if(self == NULL) return NULL;
    var_t* args = get_func_args(env);
    node_t* tn = var_array_get(args, 0);
    var_t* target = (tn != NULL) ? tn->var : NULL;
    if(target != NULL) io_schedule(vm, data, self, target);
    return NULL;
}

/* ResizeObserver delivery. The embedder can report the current laid-out border
 * box, which is sufficient for the common responsive-component contract:
 * observe an element, read contentRect/borderBoxSize, then store a pixel CSS
 * variable. A delayed first delivery lets the initial document layout finish;
 * subsequent DOM writes still trigger normal relayout before paint. */
typedef struct ro_disp { var_t* observer; var_t* target; } ro_disp_t;

static var_t* ro_size(vm_t* vm, int w, int h) {
    var_t* size = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(size, "inlineSize", var_new_int(vm, w));
    var_add(size, "blockSize", var_new_int(vm, h));
    return size;
}

static var_t* ro_size_array(vm_t* vm, int w, int h) {
    var_t* arr = var_new_array(vm);
    var_array_add(arr, ro_size(vm, w, h));
    return arr;
}

static var_t* native_ro_dispatch(vm_t* vm, var_t* env, void* data) {
    (void)env;
    ro_disp_t* d = (ro_disp_t*)data;
    if(d == NULL) return NULL;
    var_t* observer = d->observer;
    var_t* target = d->target;
    mario_free(d);
    if(observer == NULL || observer->status <= V_ST_GC_FREE ||
       target == NULL || target->status <= V_ST_GC_FREE) {
        if(getenv("MARIO_RODBG") != NULL)
            fprintf(stderr, "[rodbg] dispatch dropped observer=%p target=%p\n",
                (void*)observer, (void*)target);
        return NULL;
    }
    var_t* dead = var_find_own_member_var(observer, OB_DEAD);
    if(dead != NULL && var_get_int(dead) != 0) {
        if(getenv("MARIO_RODBG") != NULL)
            fprintf(stderr, "[rodbg] dispatch disconnected observer=%p\n", (void*)observer);
        return NULL;
    }
    var_t* cb = var_find_own_member_var(observer, OB_CB);
    if(cb == NULL || !cb->is_func) {
        if(getenv("MARIO_RODBG") != NULL)
            fprintf(stderr, "[rodbg] dispatch missing callback observer=%p\n", (void*)observer);
        return NULL;
    }

    int x = 0, y = 0, w = 0, h = 0;
    el_viewport_rect(state_from_vm(vm), handle_from_this(target), &x, &y, &w, &h);
    if(getenv("MARIO_RODBG") != NULL)
        fprintf(stderr, "[rodbg] dispatch observer=%p target=%p rect=%d,%d %dx%d\n",
            (void*)observer, (void*)target, x, y, w, h);
    var_t* rect = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(rect, "x", var_new_int(vm, x));
    var_add(rect, "y", var_new_int(vm, y));
    var_add(rect, "width", var_new_int(vm, w));
    var_add(rect, "height", var_new_int(vm, h));
    var_add(rect, "top", var_new_int(vm, y));
    var_add(rect, "right", var_new_int(vm, x + w));
    var_add(rect, "bottom", var_new_int(vm, y + h));
    var_add(rect, "left", var_new_int(vm, x));

    var_t* entry = var_new_obj_no_proto(vm, NULL, NULL);
    var_add(entry, "target", target);
    var_add(entry, "contentRect", rect);
    var_add(entry, "borderBoxSize", ro_size_array(vm, w, h));
    var_add(entry, "contentBoxSize", ro_size_array(vm, w, h));
    var_add(entry, "devicePixelContentBoxSize", ro_size_array(vm, w, h));
    var_t* entries = var_new_array(vm);
    var_array_add(entries, entry);
    var_t* args = var_new_array(vm);
    var_array_add(args, entries);
    var_array_add(args, observer);
    var_array_reverse(args);
    var_t* r = call_m_func(vm, observer, cb, args);
    if(getenv("MARIO_RODBG") != NULL)
        fprintf(stderr, "[rodbg] callback complete observer=%p result=%p\n",
            (void*)observer, (void*)r);
    if(r != NULL) var_unref(r);
    var_unref(args);
    return NULL;
}

static void ro_schedule(vm_t* vm, void* bridge, var_t* observer, var_t* target) {
    js_dom_state* st = state_any(vm, bridge);
    if(st == NULL || observer == NULL || target == NULL) return;
    ro_disp_t* d = (ro_disp_t*)mario_malloc(sizeof(ro_disp_t));
    if(d == NULL) return;
    d->observer = observer;
    d->target = target;
    var_t* tr = var_new_native_func(vm, native_ro_dispatch, d);
    int id = js_add_timer(vm, st, tr, 64, false, false, false);
    if(getenv("MARIO_RODBG") != NULL)
        fprintf(stderr, "[rodbg] schedule observer=%p target=%p timer=%d\n",
            (void*)observer, (void*)target, id);
    if(id == 0) { mario_free(d); var_unref(tr); }
}

static var_t* native_ro_observe(vm_t* vm, var_t* env, void* data) {
    var_t* r = native_observer_observe(vm, env, data);
    if(r != NULL) var_unref(r);
    var_t* self = get_obj(env, THIS);
    if(self == NULL) return NULL;
    var_t* args = get_func_args(env);
    node_t* tn = var_array_get(args, 0);
    var_t* target = (tn != NULL) ? tn->var : NULL;
    if(target != NULL) ro_schedule(vm, data, self, target);
    return NULL;
}

static var_t* native_observer_unobserve(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = get_obj(env, THIS);
    if(self == NULL) return NULL;
    var_t* args = get_func_args(env);
    node_t* tn = var_array_get(args, 0);
    var_t* target = (tn != NULL) ? tn->var : NULL;
    var_t* tg = var_find_own_member_var(self, OB_TG);
    var_t* op = var_find_own_member_var(self, OB_OPT);
    if(tg == NULL || target == NULL) return NULL;
    vm->gc.gc_defer++;
    var_t* ftg = var_new_array(vm);
    var_t* fop = var_new_array(vm);
    uint32_t sz = var_array_size(tg);
    for(uint32_t i = 0; i < sz; ++i) {
        node_t* nd = var_array_get(tg, (int32_t)i);
        node_t* od = (op != NULL) ? var_array_get(op, (int32_t)i) : NULL;
        if(nd != NULL && nd->var != NULL && nd->var != target) {
            var_array_add(ftg, nd->var);
            var_array_add(fop, (od != NULL && od->var != NULL) ? od->var : var_new(vm));
        }
    }
    ob_hide(var_add(self, OB_TG, ftg));
    ob_hide(var_add(self, OB_OPT, fop));
    vm->gc.gc_defer--;
    return NULL;
}

static var_t* native_observer_disconnect(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = get_obj(env, THIS);
    if(self == NULL) return NULL;
    var_add(self, OB_DEAD, var_new_int(vm, 1));
    ob_hide(var_add(self, OB_TG, var_new_array(vm)));   /* stop matching writes */
    ob_hide(var_add(self, OB_OPT, var_new_array(vm)));
    var_t* tid = var_find_own_member_var(self, OB_TIMER);
    /* No dispatch armed -> unroot now. Otherwise stay rooted so the in-flight
     * trampoline's d->observer remains valid; native_mo_dispatch unanchors. */
    if(tid == NULL || var_get_int(tid) == 0) mo_unanchor(vm, self);
    return NULL;
}

static var_t* native_observer_takeRecords(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* self = get_obj(env, THIS);
    if(self == NULL) return var_new_array(vm);
    var_t* pend = var_find_own_member_var(self, OB_PEND);
    if(pend == NULL) return var_new_array(vm);
    /* Hand out the buffered records and reset, per spec. */
    ob_hide(var_add(self, OB_PEND, var_new_array(vm)));
    return pend;
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

/* Element.attributes: a per-element NamedNodeMap stand-in - `length` plus
 * indexed {name, value} Attr nodes. The map object is CACHED on the bridge
 * (keyed by the element handle, exactly like dataset) so every `.attributes`
 * read of an element returns the SAME object, and it is refilled from the live
 * element on each read. removeAttributeNode/setAttributeNode also refill it, so
 * a script that caches the map and mutates through it terminates: React's
 * hydration/host-reset clear loop `e = node.attributes; for(; e.length;)
 * node.removeAttributeNode(e[0])` relies on `e.length` shrinking as attributes
 * go, which a one-shot snapshot would never do (infinite loop). */
static void attr_map_fill(vm_t* vm, js_dom_state* st, js_element_t el, var_t* map) {
    var_remove_all(map);
    int n = 0;
    vm->gc.gc_defer++;
    if(st != NULL && el != NULL && st->cb.el_attr_snapshot != NULL) {
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
                var_add(a, "nodeName", var_new_str(vm, p));
                var_add(a, "value", var_new_str(vm, sep + 1));
                var_add(a, "nodeValue", var_new_str(vm, sep + 1));
                var_add(a, "specified", var_new_bool(vm, true));
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
}

/* The cached NamedNodeMap for `el` (created on first use), refilled to match
 * the element's current attributes. Shared by the .attributes accessor and the
 * Attr-node mutators so they all hand back the one live object. */
static var_t* attr_map_for(vm_t* vm, js_dom_state* st, js_element_t el, var_t* bridge) {
    /* The .attributes accessor can be invoked through the GET path where a
     * native's `data` is NULL (only method-call paths hand it the bridge), so
     * never trust the caller's bridge - fall back to the one anchored on
     * vm->root. Caching is load-bearing: React's clear loop captures the map
     * once (e = node.attributes) then relies on e.length shrinking as
     * removeAttributeNode refills THAT SAME object; a non-cached map would spin
     * forever. st != NULL implies the bridge exists on the root, so this only
     * degrades if the root was somehow torn down mid-call. */
    if(bridge == NULL) bridge = dom_bridge_var(vm);
    if(bridge == NULL) {
        var_t* m = var_new_obj_no_proto(vm, NULL, NULL);
        attr_map_fill(vm, st, el, m);
        return m;
    }
    var_t* bag = var_find_own_member_var(bridge, "@@attrmaps");
    if(bag == NULL) {
        bag = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(bridge, "@@attrmaps", bag);
    }
    char key[32];
    snprintf(key, sizeof(key), "%p", el);
    var_t* map = var_find_own_member_var(bag, key);
    if(map == NULL) {
        map = var_new_obj_no_proto(vm, NULL, NULL);
        var_add(bag, key, map);
    }
    attr_map_fill(vm, st, el, map);
    return map;
}

static var_t* native_el_get_attributes(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    if(st == NULL || el == NULL) return var_new_null(vm);
    return var_ref(attr_map_for(vm, st, el, dom_bridge_var(vm)));
}

/* Element.removeAttributeNode(attr): drop the attribute the Attr node names
 * (accepting one of our snapshot nodes, or a bare string), refresh the cached
 * NamedNodeMap so its length shrinks, and return the removed node. */
static var_t* native_el_removeAttributeNode(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    var_t* attr = js_arg(env, 0);
    if(st == NULL || el == NULL || attr == NULL) return var_new_null(vm);
    mstr_t* s = mstr_new("");
    const char* name = NULL;
    var_t* nm = var_find_member_var(attr, "name");
    if(nm == NULL) nm = var_find_member_var(attr, "nodeName");
    if(nm != NULL) name = js_cstr(nm, s);
    else name = js_cstr(attr, s);   /* a bare string argument */
    if(name != NULL && name[0] != 0 && st->cb.el_remove_attr != NULL)
        st->cb.el_remove_attr(st->ctx, el, name);
    mstr_free(s);
    attr_map_for(vm, st, el, dom_bridge_var(vm));   /* refresh the live map React holds */
    return var_ref(attr);
}

/* Element.getAttributeNode(name): an Attr node {name, value, ...} or null. */
static var_t* native_el_getAttributeNode(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    var_t* r = var_new_null(vm);
    if(st != NULL && el != NULL && name[0] != 0 && st->cb.el_get_attr != NULL) {
        char* v = st->cb.el_get_attr(st->ctx, el, name);
        if(v != NULL) {
            r = var_new_obj_no_proto(vm, NULL, NULL);
            var_add(r, "name", var_new_str(vm, name));
            var_add(r, "nodeName", var_new_str(vm, name));
            var_add(r, "value", var_new_str(vm, v));
            var_add(r, "nodeValue", var_new_str(vm, v));
            var_add(r, "specified", var_new_bool(vm, true));
            mario_free(v);
        }
    }
    mstr_free(s);
    return r;
}

/* Element.setAttributeNode(attr): set the attribute from the node's name/value,
 * refresh the cached map, and return the previous Attr node (or null). */
static var_t* native_el_setAttributeNode(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    js_element_t el = this_handle(env);
    var_t* attr = js_arg(env, 0);
    if(st == NULL || el == NULL || attr == NULL) return var_new_null(vm);
    mstr_t* ns = mstr_new("");
    mstr_t* vs = mstr_new("");
    var_t* nm = var_find_member_var(attr, "name");
    if(nm == NULL) nm = var_find_member_var(attr, "nodeName");
    var_t* vv = var_find_member_var(attr, "value");
    if(vv == NULL) vv = var_find_member_var(attr, "nodeValue");
    if(nm != NULL) var_to_str(nm, ns);
    if(vv != NULL) var_to_str(vv, vs);
    var_t* prev = var_new_null(vm);
    if(ns->cstr[0] != 0 && st->cb.el_get_attr != NULL) {
        char* old = st->cb.el_get_attr(st->ctx, el, ns->cstr);
        if(old != NULL) {
            prev = var_new_obj_no_proto(vm, NULL, NULL);
            var_add(prev, "name", var_new_str(vm, ns->cstr));
            var_add(prev, "nodeName", var_new_str(vm, ns->cstr));
            var_add(prev, "value", var_new_str(vm, old));
            var_add(prev, "nodeValue", var_new_str(vm, old));
            var_add(prev, "specified", var_new_bool(vm, true));
            mario_free(old);
        }
    }
    if(ns->cstr[0] != 0 && st->cb.el_set_attr != NULL) {
        st->cb.el_set_attr(st->ctx, el, ns->cstr, vs->cstr);
        mo_notify_charattr(vm, el, 0, ns->cstr);   /* MutationObserver: attributes */
    }
    mstr_free(ns);
    mstr_free(vs);
    attr_map_for(vm, st, el, dom_bridge_var(vm));
    return prev;
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

/* ------------------------------------------------------------------ */
/* customElements                                                      */
/*                                                                     */
/* Minimal registry: define() records the constructor and, like the    */
/* real registry, upgrades every matching element already live in the  */
/* document by running its connectedCallback with the element as       */
/* `this`. Without that upgrade an SSR'd custom element (github.com's  */
/* <react-partial>) never reads its embedded JSON island and the       */
/* server-side skeleton stays on screen forever. The constructor is    */
/* not re-run: the bridge wrapper for the live element already exists  */
/* and re-constructing would orphan it.                                */
/* ------------------------------------------------------------------ */

static var_t* ce_registry(vm_t* vm) {
    var_t* reg = var_find_member_var(vm->root, "@@customelements");
    if(reg == NULL) {
        reg = var_new_obj_no_proto(vm, NULL, NULL);
        node_t* rn = var_add(vm->root, "@@customelements", reg);
        if(rn != NULL) { rn->invisable = 1; rn->be_unenumerable = 1; }
    }
    return reg;
}

/* A real custom-element upgrade sets the element's [[Prototype]] to the element
 * class prototype so instance methods, accessors and ES2022 private methods
 * (`this.#i()`) resolve on the live element.  mario builds every Element wrapper
 * on the shared "Element" prototype, so without this reparent the class body's
 * `this.#i()` / `this.name` never resolve and connectedCallback throws "can not
 * find function '#i' on object{...}" (React then never hydrates).  Only reparent
 * when the class prototype chain still reaches the wrapper's current prototype,
 * so the native Element methods (getAttribute, appendChild, ...) stay reachable
 * after the move - this relies on HTMLElement.prototype being linked to
 * Element.prototype (see js_register_dom_natives). */
/* A `class X extends HTMLElement` whose extends-link was dropped at class-creation
 * time (the bundle's HTMLElement binding had no prototype yet, so var_set_father
 * left X.prototype chained straight to the Object terminus) yields an instance
 * chain with NO Element.prototype: connectedCallback then throws "can not find
 * function 'hasAttribute'/'addEventListener'" on the live element and React's
 * error boundary blanks the code-view. Repair by re-linking the class prototype
 * to HTMLElement.prototype. Guarded narrowly so a legitimate chain is never
 * clobbered: only when the chain truly lacks Element.prototype AND the class
 * prototype points DIRECTLY at a prototype-less terminus (no intermediate
 * prototype holding class methods would be dropped) AND that terminus is not
 * itself re-linked (avoids a cycle through the shared Object prototype). */
static void ce_repair_proto_chain(vm_t* vm, var_t* ctor) {
    if(ctor == NULL || !ctor->is_class) return;
    var_t* elc = var_find_own_member_var(vm->root, "Element");
    var_t* el_proto_g = (elc != NULL) ? var_get_prototype(elc) : NULL;
    if(el_proto_g == NULL) return;
    var_t* ctor_proto = var_get_prototype(ctor);
    if(ctor_proto == NULL || ctor_proto == el_proto_g) return;
    bool reaches = false;
    int hops = 0;
    for(var_t* p = ctor_proto; p != NULL && hops++ < 64; p = var_get_prototype(p))
        if(p == el_proto_g) { reaches = true; break; }
    if(reaches) return;
    var_t* hec = var_find_own_member_var(vm->root, "HTMLElement");
    var_t* he_proto_g = (hec != NULL) ? var_get_prototype(hec) : NULL;
    var_t* link = (he_proto_g != NULL) ? he_proto_g : el_proto_g;
    var_t* opc = var_find_own_member_var(vm->root, "Object");
    var_t* obj_proto_g = (opc != NULL) ? var_get_prototype(opc) : NULL;
    /* Locate the chain terminus T (proto==NULL) and its predecessor P, then
     * splice the DOM prototypes in: below a per-class terminus (keeps any
     * mixin methods it holds) or, when the terminus IS the shared Object
     * prototype (must never be re-linked: the DOM chain ends there and
     * re-linking it would close a cycle), at the last class prototype P. */
    var_t* T = NULL;
    var_t* P = NULL;
    int h2 = 0;
    for(var_t* p = ctor_proto; p != NULL && h2++ < 64; p = var_get_prototype(p)) {
        var_t* nx = var_get_prototype(p);
        if(nx == NULL) { T = p; break; }
        P = p;
    }
    if(T == NULL || T == link) return;
    var_t* splice_at = (T == obj_proto_g) ? P : T;
    if(splice_at == NULL || splice_at == link) return;
    if(getenv("MARIO_CEDBG") != NULL) {
        node_t* cnn = var_find_own_member(ctor, "name");
        fprintf(stderr, "[cedbg] repair proto chain ctor=%s ctor_proto=%p T=%p splice_at=%p now->%p\n",
                (cnn != NULL && cnn->var != NULL) ? var_get_str(cnn) : "?",
                (void*)ctor_proto, (void*)T, (void*)splice_at, (void*)link);
    }
    var_set_prototype(splice_at, link);
}

static void ce_reparent(vm_t* vm, var_t* el, var_t* ctor) {
    if(el == NULL || ctor == NULL) return;
    ce_repair_proto_chain(vm, ctor);
    var_t* ctor_proto = var_get_prototype(ctor);
    var_t* el_proto = var_get_prototype(el);
    if(ctor_proto == NULL || el_proto == NULL || ctor_proto == el_proto) return;
    bool reaches = false;
    int hops = 0;
    for(var_t* p = ctor_proto; p != NULL && hops++ < 64; p = var_get_prototype(p))
        if(p == el_proto) { reaches = true; break; }
    /* Real-DOM semantics: reparent whenever the class prototype chain reaches the
     * GLOBAL Element.prototype, even if the element's current proto is something
     * else entirely. github's catalyst path leaves el.proto pointing at the class
     * VAR (own key "prototype") rather than X.prototype, so the old el_proto test
     * never matched and connectedCallback ran on a chain without Element methods
     * ("can not find function 'hasAttribute'"). */
    var_t* elc_g = var_find_own_member_var(vm->root, "Element");
    var_t* el_proto_g = (elc_g != NULL) ? var_get_prototype(elc_g) : NULL;
    bool reaches_g = false;
    if(el_proto_g != NULL && !reaches) {
        hops = 0;
        for(var_t* p = ctor_proto; p != NULL && hops++ < 64; p = var_get_prototype(p))
            if(p == el_proto_g) { reaches_g = true; break; }
    }
    if(getenv("MARIO_CEDBG") != NULL) {
        node_t* cnn = var_find_own_member(ctor, "name");
        fprintf(stderr, "[cedbg] reparent ctor=%s ctor_proto=%p el_proto=%p reaches=%d reaches_g=%d hops=%d\n",
                (cnn != NULL && cnn->var != NULL) ? var_get_str(cnn) : "?",
                (void*)ctor_proto, (void*)el_proto, (int)reaches, (int)reaches_g, hops);
    }
    if(reaches || reaches_g)
        var_set_prototype(el, ctor_proto);
}

/* During a custom-element upgrade the leaf constructor's `super()` must hand
 * back the LIVE wrapper.  Bundles downlevel ES2022 class fields into the
 * constructor body (`this.nameAttribute = "app-name"`, `this.#c = new Map()`),
 * and those assignments target whatever super() returned; if super() mints a
 * fresh object the upgraded element keeps none of its fields (github's
 * react-app then reads nameAttribute=undefined and a #c with no .get).  The
 * HTMLElement constructor returns this pending element while an upgrade is in
 * flight, and behaves normally (NULL -> default new object) otherwise. */
static var_t* s_ce_pending_el = NULL;
static var_t* native_htmlel_ctor(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    if(s_ce_pending_el != NULL) { var_ref(s_ce_pending_el); return s_ce_pending_el; }
    return NULL;
}

/* Run the class constructor (and @fields initializers) on an existing element
 * wrapper so that TS-downleveled private fields (WeakMap/WeakSet brands) are
 * registered before connectedCallback fires.  Without this the brand check
 * `!e.has(t)` in __classPrivateFieldSet throws "Cannot write private member to
 * an object whose class did not declare it".  Errors are suppressed: the
 * constructor may not perfectly handle an already-live element, but partial
 * initialization is better than aborting the whole registration chain. */
static void ce_init_instance(vm_t* vm, var_t* el, var_t* ctor) {
    /* Upgrade the wrapper's [[Prototype]] to the element class prototype first so
     * the constructor / @fields / lifecycle callbacks resolve instance members. */
    ce_reparent(vm, el, ctor);
    if(getenv("MARIO_CEDBG") != NULL)
        fprintf(stderr, "[cedbg] after-reparent#1 el=%p proto=%p\n",
                (void*)el, (void*)var_get_prototype(el));
    /* @fields initializers (ES2022 class fields), superclass-first. Advance
     * only in the body: a `c = NULL` loop-increment truncates the walk to the
     * leaf class and drops inherited field initializers (catalyst's
     * nameAttribute lives on the base element class). */
    var_t* chain[16];
    int cn = 0;
    for(var_t* c = ctor; c != NULL && cn < 16; ) {
        chain[cn++] = c;
        node_t* sn = var_find_own_member(c, "@super");
        c = (sn != NULL) ? sn->var : NULL;
    }
    if(getenv("MARIO_CEDBG") != NULL) {
        fprintf(stderr, "[cedbg] ce_init chain=%d", cn);
        for(int i = 0; i < cn; i++) {
            node_t* fe = var_find_own_member(chain[i], "@fields");
            fprintf(stderr, " [i%d fields=%u]", i,
                    (fe != NULL && fe->var != NULL && fe->var->is_array) ? var_array_size(fe->var) : 0);
        }
        fprintf(stderr, "\n");
    }
    for(int i = cn - 1; i >= 0; i--) {
        node_t* fe = var_find_own_member(chain[i], "@fields");
        if(fe == NULL || fe->var == NULL || !fe->var->is_array) continue;
        uint32_t fc = var_array_size(fe->var);
        for(uint32_t k = 0; k < fc; k++) {
            var_t* pair = var_array_get_var(fe->var, (int32_t)k);
            if(pair == NULL || !pair->is_array) continue;
            var_t* nmv = var_array_get_var(pair, 0);
            var_t* fnv = var_array_get_var(pair, 1);
            if(nmv == NULL || fnv == NULL || !fnv->is_func) continue;
            const char* fs = var_get_str(nmv);
            if(fs == NULL) continue;
            if(getenv("MARIO_CEDBG") != NULL)
                fprintf(stderr, "[cedbg] @field install '%s' on el\n", fs);
            var_t* rv = call_m_func(vm, el, fnv, NULL);
            if(vm->propagating_err != NULL) {
                var_unref(vm->propagating_err);
                vm->propagating_err = NULL;
                vm->abort_run = false;
            }
            bool owned = (rv != NULL); /* call_m_func returns refs>=1; var_new returns refs==0 */
            if(rv == NULL) rv = var_new(vm);
            node_t* rn = var_add(el, fs, rv);
            if(rn != NULL && owned) var_unref(rv);
        }
    }
    /* Constructor function body: call it with `this=el` so TS-downleveled
     * private-field brands (WeakMap.set(this,...)) are registered.  Errors are
     * suppressed — a constructor that expects a fresh element may partially fail
     * on an already-live wrapper, but the brand initialization (the first thing
     * most constructors do) succeeds before any DOM access. */
    var_t* proto = var_find_member_var(ctor, "prototype");
    var_t* ctor_fn = NULL;
    node_t* pn = var_find_own_member(ctor, "@@ctor");
    if(pn != NULL && pn->var != NULL && pn->var->is_func)
        ctor_fn = pn->var;
    else if(proto != NULL)
        ctor_fn = var_find_member_var(proto, "constructor");
    if(ctor_fn != NULL && ctor_fn->is_func) {
        /* Real DOM hands the element its class prototype chain BEFORE the
         * constructor body runs (github elements do super() then
         * this.addEventListener(...) in the constructor); without this the
         * constructor throws "can not find function 'addEventListener'". */
        ce_reparent(vm, el, ctor);
        if(getenv("MARIO_CEDBG") != NULL) {
            var_t* ael = var_find_member_var(el, "addEventListener");
            fprintf(stderr, "[cedbg] pre-ctor el=%p is_class=%d is_func=%d ctor_proto=%p chain:",
                    (void*)el, (int)ctor->is_class, (int)ctor->is_func,
                    (void*)var_get_prototype(ctor));
            int hh = 0;
            for(var_t* p = var_get_prototype(el); p != NULL && hh < 5; p = var_get_prototype(p))
                fprintf(stderr, " %p", (void*)p);
            fprintf(stderr, " | addEventListener=%d\n", (ael != NULL && ael->is_func) ? 1 : 0);
        }
        s_ce_pending_el = el;
        var_ref(el);
        var_t* ret = call_m_func(vm, el, ctor_fn, NULL);
        var_unref(el);
        s_ce_pending_el = NULL;
        if(ret != NULL) var_unref(ret);
        if(vm->propagating_err != NULL) {
            if(getenv("MARIO_CEDBG") != NULL) {
                mstr_t* cem = mstr_new("");
                var_to_str(vm->propagating_err, cem);
                fprintf(stderr, "[cedbg] ce_init_instance ctor FAILED: %.300s\n", cem->cstr);
                mstr_free(cem);
                const char* ctl = mario_last_throw_loc();
                if(ctl != NULL) fprintf(stderr, "[cedbg] ctor throw loc:%.300s", ctl);
            }
            var_unref(vm->propagating_err);
            vm->propagating_err = NULL;
            vm->abort_run = false;
        } else if(getenv("MARIO_CEDBG") != NULL) {
            fprintf(stderr, "[cedbg] ce_init_instance ctor OK\n");
        }
    } else if(getenv("MARIO_CEDBG") != NULL) {
        fprintf(stderr, "[cedbg] ce_init_instance NO ctor_fn (proto=%p)\n", (void*)proto);
    }
    /* The derived constructor's `super()` (the HTMLElement ctor path) hands back
     * this same live wrapper, and mario's [[Construct]] then re-asserts the
     * wrapper's ORIGINAL [[Prototype]] on it - undoing the ce_reparent above. The
     * base class's ES2022 private methods (`this.#i()` in the react-app /
     * react-partial connectedCallback) live on the class prototype chain, so once
     * the proto is knocked back to the plain Element wrapper they no longer
     * resolve and connectedCallback throws "can not find function '#i'" (React
     * never hydrates). Re-apply the reparent AFTER construction so the lifecycle
     * callbacks see the class prototype. */
    ce_reparent(vm, el, ctor);
    if(getenv("MARIO_CEDBG") != NULL)
        fprintf(stderr, "[cedbg] after-reparent#2 el=%p proto=%p\n",
                (void*)el, (void*)var_get_prototype(el));
}

static void ce_upgrade_all(vm_t* vm, js_dom_state* st, const char* name, var_t* ctor) {
    var_t* proto = var_find_member_var(ctor, "prototype");
    var_t* cb = (proto != NULL) ? var_find_member_var(proto, "connectedCallback") : NULL;
    if(getenv("MARIO_CEDBG") != NULL)
        fprintf(stderr, "[cedbg] upgrade_all name=%s proto=%p cb=%p\n",
                name ? name : "?", (void*)proto, (void*)cb);
    if(cb == NULL || !cb->is_func) return;
    var_t* els = query_elements(vm, st, NULL, name);
    uint32_t n = var_array_size(els);
    if(getenv("MARIO_CEDBG") != NULL)
        fprintf(stderr, "[cedbg] upgrade_all name=%s matched=%u\n", name ? name : "?", (unsigned)n);
    vm->gc.gc_defer++;   /* els and its wrappers stay off the root set */
    for(uint32_t i = 0; i < n; ++i) {
        var_t* el = var_array_get_var(els, i);
        if(el == NULL || var_find_own_member(el, "@@ceupg") != NULL) continue;
        node_t* un = var_add(el, "@@ceupg", var_new_bool(vm, 1));
        if(un != NULL) { un->invisable = 1; un->be_unenumerable = 1; }
        /* Initialize private-field brands before lifecycle callbacks. */
        ce_init_instance(vm, el, ctor);
        if(getenv("MARIO_CEDBG") != NULL) {
            js_element_t h = (js_element_t)el->value;
            const char* tg = (st->cb.el_get_tag != NULL) ? st->cb.el_get_tag(st->ctx, h) : NULL;
            const char* an = (st->cb.el_get_attr != NULL) ? st->cb.el_get_attr(st->ctx, h, "app-name") : NULL;
            const char* pn2 = (st->cb.el_get_attr != NULL) ? st->cb.el_get_attr(st->ctx, h, "partial-name") : NULL;
            var_t* q = query_elements(vm, st, h, "[data-target~=\"react-app.embeddedData\"]");
            var_t* q2 = query_elements(vm, st, h, "[data-target~=\"react-partial.embeddedData\"]");
            fprintf(stderr, "[cedbg] PROBE tag=%s app-name=%s partial-name=%s nameAttrField=%d embApp=%u embPart=%u\n",
                    tg ? tg : "?", an ? an : "(null)", pn2 ? pn2 : "(null)",
                    var_find_own_member(el, "nameAttribute") != NULL,
                    var_array_size(q), var_array_size(q2));
            /* Walk el's live prototype chain and report where (if anywhere) the
             * private method '#i' lives, plus whether a full member lookup from
             * el resolves it. This distinguishes "reparent didn't reach S.proto"
             * from "#i is not stored on the prototype at all". */
            {
                fprintf(stderr, "[cedbg] CHAIN el=%p find#i=%d",
                        (void*)el, var_find_member_var(el, "#i") != NULL);
                int lvl = 0;
                for(var_t* p = el; p != NULL && lvl < 8; p = var_get_prototype(p), ++lvl) {
                    node_t* mi = var_find_own_member(p, "#i");
                    node_t* cc = var_find_own_member(p, "connectedCallback");
                    fprintf(stderr, " [%d]%p(#i=%d,cc=%d)", lvl, (void*)p,
                            mi != NULL, cc != NULL);
                }
                fprintf(stderr, "\n");
            }
        }
        /* Pass element as first arg: GitHub's catalyst pattern declares
         * connectedCallback(el, prev) where `el` is the element (not `this`).
         * Standard custom elements ignore the extra arg harmlessly. */
        var_t* args = var_new_array(vm);
        var_array_add(args, el);
        var_array_reverse(args);
        var_t* ret = call_m_func(vm, el, cb, args);
        var_unref(args);
        if(ret != NULL) var_unref(ret);
        /* Suppress per-element errors so one broken upgrade doesn't abort the
         * whole registration chain (the outer define() catch re-throws). */
        if(vm->propagating_err != NULL) {
            if(getenv("MARIO_CEDBG") != NULL) {
                mstr_t* em = mstr_new("");
                var_to_str(vm->propagating_err, em);
                fprintf(stderr, "[cedbg] upgrade cb FAILED for element %u: %s\n", i, em->cstr);
                mstr_free(em);
                const char* tl = mario_last_throw_loc();
                if(tl != NULL) fprintf(stderr, "[cedbg] cb throw loc:%s", tl);
            }
            var_unref(vm->propagating_err);
            vm->propagating_err = NULL;
            vm->abort_run = false;
        } else if(getenv("MARIO_CEDBG") != NULL) {
            fprintf(stderr, "[cedbg] upgrade cb OK for element %u\n", i);
        }
    }
    vm->gc.gc_defer--;
}

/* Upgrade every registered custom element whose instances were inserted since
 * the last drain (see ce_mark_dirty / js_dom_state.ce_dirty). Called by the
 * engine tick after timers/fetch settle, which is when React's post-deferred-
 * data fill commit has appended its <relative-time> etc. Snapshot-and-clear
 * first so a connectedCallback that itself inserts custom elements simply
 * re-marks them for a later tick (bounded, no recursion here). Errors escaping
 * a lifecycle callback are swallowed per name, exactly like native_ce_define,
 * so one broken element cannot abort the page's JS. Returns non-zero if any
 * name was processed (caller marks content dirty to repaint the new text). */
int js_dom_upgrade_dirty(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    if(st == NULL || st->ce_dirty_n == 0) return 0;
    /* Honour the same defer window as define(): while the post-swap script
     * queue is still running (defer_upgrades), a connectedCallback that awaits
     * a registration a LATER document-order script performs would force the
     * synchronous await spin / re-entrant script pump that corrupts suspended
     * webpack factory frames. Hold the dirty names (do NOT clear) until the
     * queue drains, exactly like the deferred define-time upgrades. */
    if(st->defer_upgrades) return 0;
    int n = st->ce_dirty_n;
    char names[CE_DIRTY_MAX][CE_DIRTY_NAME];
    /* Bounded copy: a corrupted (non NUL-terminated) ce_dirty entry made the
     * strcpy run off into unmapped memory, and because ce_dirty_n is only
     * cleared AFTER the copy, the SEGV checkpoint recovery re-entered with the
     * same dirty list and faulted forever (engine thread pinned at 100% CPU in
     * _platform_strcpy, timers never polled again - pinterest's shell state).
     * Truncate instead; a garbage name simply finds no ctor below. */
    for(int i = 0; i < n; ++i) {
        strncpy(names[i], st->ce_dirty[i], CE_DIRTY_NAME - 1);
        names[i][CE_DIRTY_NAME - 1] = 0;
    }
    st->ce_dirty_n = 0;
    var_t* reg = ce_registry(vm);
    if(reg == NULL) return n;
    if(getenv("MARIO_CEDBG") != NULL)
        fprintf(stderr, "[cedbg] upgrade_dirty names=%d\n", n);
    for(int i = 0; i < n; ++i) {
        var_t* ctor = var_find_own_member_var(reg, names[i]);
        if(ctor != NULL)
            ce_upgrade_all(vm, st, names[i], ctor);
        if(vm->propagating_err != NULL) {
            if(getenv("MARIO_CEDBG") != NULL) {
                mstr_t* em = mstr_new("");
                var_to_str(vm->propagating_err, em);
                fprintf(stderr, "[cedbg] upgrade_dirty '%s' SUPPRESSING: %s\n", names[i], em->cstr);
                mstr_free(em);
            }
            var_unref(vm->propagating_err);
            vm->propagating_err = NULL;
            vm->abort_run = false;
        }
    }
    return n;
}

void js_dom_clear_dirty_ce(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    if(st == NULL) return;
    st->ce_dirty_n = 0;
}

/* Deferred bulk upgrades (see js_dom_state.defer_upgrades). Pairs of
 * [name, ctor] park in a hidden root array so the ctor vars survive GC until
 * the post-swap script queue finishes and js_dom_drain_deferred_upgrades runs
 * them in registration order. */
#define DOM_DEFER_UP_KEY "@@defer_up"

static var_t* ce_deferred_list(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return NULL;
    var_t* bridge = var_find_own_member_var(vm->root, DOM_BRIDGE_KEY);
    if(bridge == NULL) return NULL;
    var_t* list = var_find_own_member_var(bridge, DOM_DEFER_UP_KEY);
    if(list == NULL) {
        list = var_new_array(vm);
        node_t* n = var_add(bridge, DOM_DEFER_UP_KEY, list);
        if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
    }
    return list;
}

static void ce_defer_upgrade(vm_t* vm, const char* name, var_t* ctor) {
    var_t* list = ce_deferred_list(vm);
    if(list == NULL) return;
    var_t* pair = var_new_array(vm);
    var_array_add(pair, var_new_str(vm, name));
    var_array_add(pair, ctor);
    var_array_add(list, pair);
    /* The list node now owns the only reference to `pair` (var_array_add takes
     * it); an extra var_unref here dropped it to 0 and freed the pair, so the
     * drain later read a recycled var, never matched name/ctor, and silently
     * skipped every deferred connectedCallback upgrade (github's react-app
     * never rendered its embedded payload -> permanent grey skeletons). */
}

void js_dom_set_defer_upgrades(vm_t* vm, bool on) {
    js_dom_state* st = state_from_vm(vm);
    if(st != NULL) st->defer_upgrades = on;
}

void js_dom_drain_deferred_upgrades(vm_t* vm) {
    js_dom_state* st = state_from_vm(vm);
    if(st == NULL) return;
    st->defer_upgrades = false;
    var_t* list = ce_deferred_list(vm);
    if(getenv("MARIO_CEDBG") != NULL)
        fprintf(stderr, "[cedbg] drain list=%p size=%u\n", (void*)list,
                (list != NULL) ? (unsigned)var_array_size(list) : 0u);
    if(list == NULL) return;
    /* Snapshot every queued pair with an ownership ref BEFORE running any
     * connectedCallback: var_array_del does not compact indices, so the old
     * read-index-0-then-del-then-read-index-0 loop re-read a recycled slot
     * (nm type 0) and silently skipped nearly every deferred upgrade - which
     * is why github's react-app never rendered its embedded payload and the
     * page stayed a grey skeleton. Process the snapshot, then drop the slots
     * by descending index (each key still exists at its own del) and release
     * the snapshot refs. Bounded rounds in case a callback re-queues. */
    int rounds = 0;
    while(rounds++ < 16) {
        uint32_t n = var_array_size(list);
        if(n == 0) break;
        if(n > 128) n = 128;
        var_t* snap[128];
        for(uint32_t i = 0; i < n; ++i) {
            snap[i] = var_array_get_var(list, (int32_t)i);
            if(snap[i] != NULL) var_ref(snap[i]);
        }
        for(uint32_t i = 0; i < n; ++i) {
            var_t* pair = snap[i];
            if(pair == NULL) continue;
            var_t* nm = var_array_get_var(pair, 0);
            var_t* ctor = var_array_get_var(pair, 1);
            if(getenv("MARIO_CEDBG") != NULL)
                fprintf(stderr, "[cedbg] drain iter pair=%p nm=%p nmtype=%d ctor=%p\n",
                    (void*)pair, (void*)nm, (nm != NULL) ? (int)nm->type : -1, (void*)ctor);
            if(nm != NULL && nm->type == V_STRING && ctor != NULL)
                ce_upgrade_all(vm, st, var_get_str(nm), ctor);
        }
        for(uint32_t i = n; i-- > 0; )
            var_array_del(list, (int32_t)i);
        for(uint32_t i = 0; i < n; ++i)
            if(snap[i] != NULL) var_unref(snap[i]);
    }
}

/* define(name, ctor): record once (the real registry throws on a redefinition,
 * silently ignoring it here is closer than aborting the bundle) and upgrade. */
static var_t* native_ce_define(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    var_t* ctor = js_arg(env, 1);
    if(name != NULL && name[0] != 0 && ctor != NULL && (ctor->is_func || ctor->is_class)) {
        if(getenv("MARIO_CEDBG") != NULL)
            fprintf(stderr, "[cedbg] define '%s' is_func=%d is_class=%d\n", name,
                    (int)ctor->is_func, (int)ctor->is_class);
        var_t* reg = ce_registry(vm);
        if(reg != NULL && var_find_own_member(reg, name) == NULL) {
            var_add(reg, name, ctor);
            if(st != NULL) {
                if(getenv("MARIO_CEDBG") != NULL) {
                    var_t* dbg_els = query_elements(vm, st, NULL, name);
                    fprintf(stderr, "[cedbg] upgrade '%s' found %u elements defer=%d\n",
                            name, var_array_size(dbg_els), (int)st->defer_upgrades);
                }
                if(st->defer_upgrades)
                    ce_defer_upgrade(vm, name, ctor);
                else
                    ce_upgrade_all(vm, st, name, ctor);
            }
        }
    }
    /* Final safety net: if any nested upgrade/constructor left an error
     * propagating (e.g. a catalyst connectedCallback that calls getAttribute
     * on the wrong receiver), swallow it here so the page's try-catch around
     * define() never sees a spurious re-throw that aborts the whole bundle. */
    if(vm->propagating_err != NULL) {
        if(getenv("MARIO_CEDBG") != NULL)
            fprintf(stderr, "[cedbg] define '%s' SUPPRESSING escaped error\n", name);
        var_unref(vm->propagating_err);
        vm->propagating_err = NULL;
        vm->abort_run = false;
    }
    mstr_free(s);
    return NULL;
}

static var_t* native_ce_get(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    var_t* reg = var_find_member_var(vm->root, "@@customelements");
    var_t* ctor = (reg != NULL && name != NULL) ? var_find_member_var(reg, name) : NULL;
    mstr_free(s);
    return (ctor != NULL) ? ctor : var_new(vm);
}

/* customElements.upgrade(el): catalyst/react roots re-upgrade a subtree after
 * stamping templates; without it the registration chain aborts on the first
 * call (github-elements' connectedCallback path). */
static var_t* native_ce_upgrade(vm_t* vm, var_t* env, void* data) {
    js_dom_state* st = state_any(vm, data);
    var_t* elv = js_arg(env, 0);
    if(st == NULL || elv == NULL || st->cb.el_get_tag == NULL) return NULL;
    js_element_t el = handle_from_this(elv);
    if(el == NULL) return NULL;
    char* tag = st->cb.el_get_tag(st->ctx, el);
    if(tag == NULL) return NULL;
    for(char* p = tag; *p != 0; ++p)
        if(*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    var_t* reg = ce_registry(vm);
    var_t* ctor = (reg != NULL) ? var_find_member_var(reg, tag) : NULL;
    mario_free(tag);
    if(ctor == NULL) return NULL;
    var_t* proto = var_find_member_var(ctor, "prototype");
    var_t* cb = (proto != NULL) ? var_find_member_var(proto, "connectedCallback") : NULL;
    if(cb == NULL || !cb->is_func) return NULL;
    if(var_find_own_member(elv, "@@ceupg") != NULL) return NULL;
    node_t* un = var_add(elv, "@@ceupg", var_new_bool(vm, 1));
    if(un != NULL) { un->invisable = 1; un->be_unenumerable = 1; }
    ce_init_instance(vm, elv, ctor);
    var_t* args = var_new_array(vm);
    var_array_add(args, elv);
    var_array_reverse(args);
    var_t* ret = call_m_func(vm, elv, cb, args);
    var_unref(args);
    if(ret != NULL) var_unref(ret);
    if(vm->propagating_err != NULL) {
        var_unref(vm->propagating_err);
        vm->propagating_err = NULL;
        vm->abort_run = false;
    }
    return NULL;
}

/* whenDefined(): the definition can only land later through another define,
 * but every bundle on github.com awaits it right after registering, so an
 * immediately-settled thenable keeps the chain moving. */
static var_t* ce_then(vm_t* vm, var_t* env, void* data) {
    var_t* fn = js_arg(env, 0);
    var_t* ctor = (var_t*)data;
    if(fn != NULL && fn->is_func) {
        var_t* args = var_new_array(vm);
        if(ctor != NULL) var_array_add(args, ctor);
        var_t* ret = call_m_func(vm, NULL, fn, args);
        var_unref(args);
        if(ret != NULL) var_unref(ret);
    }
    return NULL;
}

static var_t* ce_catch(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data;
    return NULL;
}

static var_t* native_ce_whenDefined(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* s = mstr_new("");
    const char* name = js_arg_cstr(env, 0, s);
    var_t* reg = var_find_member_var(vm->root, "@@customelements");
    var_t* ctor = (reg != NULL && name != NULL) ? var_find_member_var(reg, name) : NULL;
    var_t* p = var_new_obj_no_proto(vm, NULL, NULL);
    vm_reg_static(vm, p, "then(fn)", ce_then, ctor);
    vm_reg_static(vm, p, "catch(fn)", ce_catch, NULL);
    mstr_free(s);
    return p;
}

bool js_register_dom_natives(vm_t* vm, void* ctx, const js_dom_callbacks_t* cb) {
    if(vm == NULL || cb == NULL) return false;

    /* Allocate the bridge state and hang it off vm->root so that natives
     * invoked through method-call paths (where `data` may be the env) can
     * still recover it via state_from_vm(). */
    js_dom_state* st = (js_dom_state*)mario_malloc(sizeof(js_dom_state));
    if(st == NULL) return false;
    /* Zero the WHOLE state first: timer_drops / defer_upgrades / ce_dirty_n
     * were left uninitialized once, and a garbage ce_dirty_n made
     * js_dom_upgrade_dirty walk its fixed 48-entry stack/heap tables hundreds
     * of entries deep (ASan: stack-buffer-overflow writes into names[][] plus
     * a heap-buffer-overflow read past the state block). That stack scribble
     * is a corruption source for the teardown crash family. memset also
     * future-proofs any field added without a matching init line below. */
    memset(st, 0, sizeof(js_dom_state));
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
    st->dom_loaded     = false;
    st->parsing_complete = false;
    st->in_async_cb    = false;

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
    vm_reg_native(vm, doc_cls, "elementFromPoint(x,y)", native_document_elementFromPoint, bridge);
    /* title / body as accessor properties so `document.title` and
     * `document.body.innerHTML` resolve like a real browser. */
    reg_accessor(vm, doc_cls, "title", native_document_get_title, native_document_set_title, bridge);
    reg_accessor(vm, doc_cls, "body", native_document_get_body, NULL, bridge);
    reg_accessor(vm, doc_cls, "activeElement", native_document_get_activeElement, NULL, bridge);
    reg_accessor(vm, doc_cls, "currentScript", native_document_get_currentScript, NULL, bridge);
    reg_accessor(vm, doc_cls, "scripts", native_document_get_scripts, NULL, bridge);
    reg_accessor(vm, doc_cls, "head", native_document_get_head, NULL, bridge);
    reg_accessor(vm, doc_cls, "documentElement", native_document_get_documentElement, NULL, bridge);
    /* Child traversal on the Document itself (see the natives above): without
     * these the inherited Element walk yields an empty list and React hydrates
     * nothing. */
    reg_accessor(vm, doc_cls, "firstChild", native_document_get_firstChild, NULL, bridge);
    reg_accessor(vm, doc_cls, "lastChild", native_document_get_lastChild, NULL, bridge);
    reg_accessor(vm, doc_cls, "firstElementChild", native_document_get_firstElementChild, NULL, bridge);
    reg_accessor(vm, doc_cls, "lastElementChild", native_document_get_lastElementChild, NULL, bridge);
    reg_accessor(vm, doc_cls, "childNodes", native_document_get_childNodes, NULL, bridge);
    reg_accessor(vm, doc_cls, "children", native_document_get_children, NULL, bridge);
    reg_accessor(vm, doc_cls, "childElementCount", native_document_get_childElementCount, NULL, bridge);
    reg_accessor(vm, doc_cls, "URL", native_document_get_url, NULL, bridge);
    reg_accessor(vm, doc_cls, "documentURI", native_document_get_url, NULL, bridge);
    /* Node.baseURI: with no <base> support the document URL is the base. */
    reg_accessor(vm, doc_cls, "baseURI", native_document_get_url, NULL, bridge);
    reg_accessor(vm, doc_cls, "readyState", native_document_get_readyState, NULL, bridge);
    reg_accessor(vm, doc_cls, "domain", native_document_get_empty_str, NULL, bridge);
    reg_accessor(vm, doc_cls, "referrer", native_document_get_empty_str, NULL, bridge);
    reg_accessor(vm, doc_cls, "characterSet", native_document_get_charset, NULL, bridge);
    reg_accessor(vm, doc_cls, "fonts", native_document_get_fonts, NULL, bridge);
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
    /* Real DOM hierarchy: HTMLElement.prototype inherits Element.prototype (which
     * inherits Node/EventTarget), so a `class X extends HTMLElement` custom element
     * resolves the native Element methods (getAttribute, appendChild, ...) up its
     * prototype chain.  mario created HTMLElement as a standalone class extending
     * Object, leaving the chain without Element - and ce_reparent (custom-element
     * upgrade) would then strip every Element method off the upgraded wrapper.
     * Link HTMLElement.prototype -> Element.prototype so both instance methods and
     * the class's own members stay reachable after an upgrade. */
    var_set_prototype(var_get_prototype(htmlel_cls), el_proto);
    /* Upgrade-aware constructor: see native_htmlel_ctor / s_ce_pending_el. */
    {
        var_t* hctor = var_new_native_func(vm, native_htmlel_ctor, NULL);
        if(hctor != NULL) {
            /* var_new_native_func returns an UNOWNED var (refs==0, like var_new);
             * var_add's @@ctor node adopts the sole reference (refs 0->1). Do NOT
             * var_unref here: the old unconditional unref dropped it to 0 and freed
             * the ctor out from under the @@ctor node, dangling its ->var. That block
             * was then recycled into an unrelated live var, and the teardown
             * node_free(@@ctor) stole a reference from it (a cross-object refcount
             * underflow). Mirrors the `owned` guard idiom at the @fields install
             * above - only release a var the caller actually owned. */
            var_add(htmlel_cls, "@@ctor", hctor);
        }
    }
    /* HTMLCanvasElement: the canvas bridge hangs getContext()/width/height on
     * the Element class (see js_canvas.c), but `HTMLCanvasElement` itself is a
     * standard global that feature tests and instanceof checks read. Expose it
     * with its prototype in the Element chain so both resolve. */
    {
        var_t* canvas_cls = vm_new_class(vm, "HTMLCanvasElement");
        if(canvas_cls != NULL)
            var_set_prototype(var_get_prototype(canvas_cls), el_proto);
    }
    /* FontFace: see the FontFaceSet note above - constructible and inert, but
     * `new FontFace(...).load()` has to resolve instead of throwing. */
    {
        var_t* ff_cls = vm_new_class(vm, "FontFace");
        if(ff_cls != NULL) {
            vm_reg_native(vm, ff_cls, "constructor(family, src, desc)", native_FontFace_ctor, bridge);
            vm_reg_native(vm, ff_cls, "load()", native_FontFace_load, bridge);
        }
    }
    /* The same references appear for the other IDL bases web bundles test or
     * subclass (SVG markup, fragments, observers). vm_new_class reuses an
     * existing live binding, so these are no-ops if a real impl is registered
     * elsewhere (e.g. Event/CustomEvent in js_event.c). */
    vm_new_class(vm, "SVGElement");
    var_t* frag_cls   = vm_new_class(vm, "DocumentFragment");
    /* Observers ship a working method surface (see native_observer_*): an
     * empty class makes observe() throw and kills the calling boot chain. */
    var_t* mo_cls = vm_new_class(vm, "MutationObserver");
    vm_reg_native(vm, mo_cls, "constructor(cb)", native_observer_ctor, bridge);
    vm_reg_native(vm, mo_cls, "observe(target, options)", native_observer_observe, bridge);
    vm_reg_native(vm, mo_cls, "disconnect()", native_observer_disconnect, bridge);
    vm_reg_native(vm, mo_cls, "takeRecords()", native_observer_takeRecords, bridge);
    var_t* io_cls = vm_new_class(vm, "IntersectionObserver");
    vm_reg_native(vm, io_cls, "constructor(cb, options)", native_observer_ctor, bridge);
    vm_reg_native(vm, io_cls, "observe(target, options)", native_io_observe, bridge);
    vm_reg_native(vm, io_cls, "unobserve(target)", native_observer_unobserve, bridge);
    vm_reg_native(vm, io_cls, "disconnect()", native_observer_disconnect, bridge);
    vm_reg_native(vm, io_cls, "takeRecords()", native_observer_takeRecords, bridge);
    var_t* ro_cls = vm_new_class(vm, "ResizeObserver");
    vm_reg_native(vm, ro_cls, "constructor(cb)", native_observer_ctor, bridge);
    vm_reg_native(vm, ro_cls, "observe(target, options)", native_ro_observe, bridge);
    vm_reg_native(vm, ro_cls, "unobserve(target)", native_observer_unobserve, bridge);
    vm_reg_native(vm, ro_cls, "disconnect()", native_observer_disconnect, bridge);
    vm_reg_native(vm, ro_cls, "takeRecords()", native_observer_takeRecords, bridge);
    reg_accessor(vm, el_cls, "textContent", native_el_get_textContent, native_el_set_textContent, bridge);
    reg_accessor(vm, el_cls, "nodeValue", native_el_get_nodeValue, native_el_set_nodeValue, bridge);
    reg_accessor(vm, el_cls, "data", native_el_get_nodeValue, native_el_set_nodeValue, bridge);
    reg_accessor(vm, el_cls, "innerText", native_el_get_innerText, native_el_set_innerText, bridge);
    reg_accessor(vm, el_cls, "innerHTML", native_el_get_innerHTML, native_el_set_innerHTML, bridge);
    reg_accessor(vm, el_cls, "tagName", native_el_get_tagName, NULL, bridge);
    reg_accessor(vm, el_cls, "nodeName", native_el_get_tagName, NULL, bridge);
    reg_accessor(vm, el_cls, "localName", native_el_get_localName, NULL, bridge);
    reg_accessor(vm, el_cls, "nodeType", native_el_get_nodeType, NULL, bridge);
    /* Node.baseURI on every element (document URL; no <base> support). */
    reg_accessor(vm, el_cls, "baseURI", native_document_get_url, NULL, bridge);
    reg_accessor(vm, el_cls, "className", native_el_get_className, native_el_set_className, bridge);
    reg_accessor(vm, el_cls, "classList", native_el_get_classList, NULL, bridge);
    reg_accessor(vm, el_cls, "style", native_el_get_style, NULL, bridge);
    vm_reg_native(vm, el_cls, "getAttribute(name)", native_el_getAttribute, bridge);
    vm_reg_native(vm, el_cls, "setAttribute(name, value)", native_el_setAttribute, bridge);
    vm_reg_native(vm, el_cls, "hasAttribute(name)", native_el_hasAttribute, bridge);
    vm_reg_native(vm, el_cls, "removeAttribute(name)", native_el_removeAttribute, bridge);
    vm_reg_native(vm, el_cls, "toggleAttribute(name, force)", native_el_toggleAttribute, bridge);
    /* Attr-node API: React's hydration/host-reset clear loop drives
     * `.attributes` + removeAttributeNode; getAttributeNode/setAttributeNode
     * complete the DOM contract scripts (and React's property diffing) expect. */
    vm_reg_native(vm, el_cls, "getAttributeNode(name)", native_el_getAttributeNode, bridge);
    vm_reg_native(vm, el_cls, "setAttributeNode(attr)", native_el_setAttributeNode, bridge);
    vm_reg_native(vm, el_cls, "removeAttributeNode(attr)", native_el_removeAttributeNode, bridge);

    /* Attribute-backed and boolean properties: one accessor pair per table
     * row, with the row index as the native's `data`. */
    for(int i = 0; i < ATTR_PROP_COUNT; ++i)
        js_acc_cls(vm, el_cls, kAttrProps[i].js, attr_prop_get, attr_prop_set,
                   (void*)(intptr_t)i);
    for(int i = 0; i < BOOL_PROP_COUNT; ++i)
        js_acc_cls(vm, el_cls, kBoolProps[i].js, bool_prop_get, bool_prop_set,
                   (void*)(intptr_t)i);

    /* `content` is special-cased (template -> DocumentFragment, else the
     * reflected attribute), so it is registered here rather than in kAttrProps. */
    reg_accessor(vm, el_cls, "content", native_el_get_content, native_el_set_content, bridge);

    /* Shadow DOM surface (degenerate light-DOM root): catalyst patches and calls
     * attachShadow/attachInternals unconditionally, so they must exist and return
     * a usable root/internals or connectedCallback throws (see the natives). */
    vm_reg_native(vm, el_cls, "attachShadow(init)", native_el_attachShadow, bridge);
    vm_reg_native(vm, el_cls, "attachInternals()", native_el_attachInternals, bridge);
    reg_accessor(vm, el_cls, "shadowRoot", native_el_get_shadowRoot, NULL, bridge);

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
    vm_reg_native(vm, el_cls, "prepend(node)", native_el_prepend, bridge);
    vm_reg_native(vm, el_cls, "insertBefore(node, ref)", native_el_insertBefore, bridge);
    vm_reg_native(vm, el_cls, "removeChild(node)", native_el_removeChild, bridge);
    vm_reg_native(vm, el_cls, "replaceChild(node, old)", native_el_replaceChild, bridge);
    vm_reg_native(vm, el_cls, "cloneNode(deep)", native_el_cloneNode, bridge);
    vm_reg_native(vm, el_cls, "remove()", native_el_remove, bridge);
    vm_reg_native(vm, el_cls, "replaceWith(node)", native_el_replaceWith, bridge);
    vm_reg_native(vm, el_cls, "before(node)", native_el_before, bridge);
    vm_reg_native(vm, el_cls, "after(node)", native_el_after, bridge);
    vm_reg_native(vm, el_cls, "replaceChildren(node)", native_el_replaceChildren, bridge);
    vm_reg_native(vm, el_cls, "contains(node)", native_el_contains, bridge);
    vm_reg_native(vm, el_cls, "isEqualNode(node)", native_el_isEqualNode, bridge);

    /* HTMLMediaElement surface (<video>/<audio>): no media pipeline exists,
     * but autoplay guards call load()/play()/pause() and a miss aborts the
     * calling effect ("can not find function 'load'", rokid's video hero). */
    vm_reg_native(vm, el_cls, "load()", native_el_media_noop, bridge);
    vm_reg_native(vm, el_cls, "play()", native_el_media_play, bridge);
    vm_reg_native(vm, el_cls, "pause()", native_el_media_noop, bridge);

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
            /* doc_cls keeps its OWN child-traversal accessors (registered
             * above): the Document singleton carries no element handle, so
             * the generic handle-based walk always yields null/empty for it,
             * while native_document_get_firstChild/childNodes expose the
             * documentElement. Re-registering the generic ones here shadowed
             * the document-specific natives (reg_accessor overwrites), and
             * React's hydrateRoot(document) - which seeds its hydration
             * cursor from document.firstChild - then saw an empty container,
             * never descended into <html>, and recovered with #418-per-host
             * + #423 (rokid: SSR main deleted, client main appended after the
             * footer, whole layout inverted). */
            if(nc != doc_cls) {
                reg_accessor(vm, nc, "childNodes", native_el_get_childNodes, NULL, bridge);
                reg_accessor(vm, nc, "children", native_el_get_children, NULL, bridge);
                reg_accessor(vm, nc, "firstChild", native_el_get_firstChild, NULL, bridge);
                reg_accessor(vm, nc, "lastChild", native_el_get_lastChild, NULL, bridge);
            }
            vm_reg_native(vm, nc, "appendChild(node)", native_el_appendChild, bridge);
            vm_reg_native(vm, nc, "append(node)", native_el_append, bridge);
            vm_reg_native(vm, nc, "prepend(node)", native_el_prepend, bridge);
            vm_reg_native(vm, nc, "insertBefore(node, ref)", native_el_insertBefore, bridge);
            vm_reg_native(vm, nc, "removeChild(node)", native_el_removeChild, bridge);
            vm_reg_native(vm, nc, "replaceChild(node, old)", native_el_replaceChild, bridge);
            vm_reg_native(vm, nc, "cloneNode(deep)", native_el_cloneNode, bridge);
            vm_reg_native(vm, nc, "remove()", native_el_remove, bridge);
            vm_reg_native(vm, nc, "replaceWith(node)", native_el_replaceWith, bridge);
            vm_reg_native(vm, nc, "before(node)", native_el_before, bridge);
            vm_reg_native(vm, nc, "after(node)", native_el_after, bridge);
            vm_reg_native(vm, nc, "replaceChildren(node)", native_el_replaceChildren, bridge);
            vm_reg_native(vm, nc, "contains(node)", native_el_contains, bridge);
            vm_reg_native(vm, nc, "isEqualNode(node)", native_el_isEqualNode, bridge);
            /* Media stubs on the HTMLElement mirror too: a <video> obtained via
             * createElement may be wrapped against htmlel_cls rather than
             * el_cls. Skipped for doc/frag/node - they are never media. */
            if(nc == htmlel_cls) {
                vm_reg_native(vm, nc, "load()", native_el_media_noop, bridge);
                vm_reg_native(vm, nc, "play()", native_el_media_play, bridge);
                vm_reg_native(vm, nc, "pause()", native_el_media_noop, bridge);
            }
            /* Query surface: DocumentFragment/HTMLElement support querySelector
             * in the real DOM. A template's `.content` fragment
             * (native_el_get_content) is queried right after cloning
             * (`h.content.cloneNode(true).querySelector(".handle")`), so the
             * fragment class needs these too, not just Element. doc_cls is
             * EXCLUDED: document.querySelector/All are registered above with the
             * document-specific natives (whole-document search) and must not be
             * shadowed by the element-handle versions. */
            if(nc != doc_cls) {
                vm_reg_native(vm, nc, "querySelector(sel)", native_el_querySelector, bridge);
                vm_reg_native(vm, nc, "querySelectorAll(sel)", native_el_querySelectorAll, bridge);
                vm_reg_native(vm, nc, "getElementsByTagName(tag)", native_el_getElementsByTagName, bridge);
                vm_reg_native(vm, nc, "getElementsByClassName(cls)", native_el_getElementsByClassName, bridge);
                vm_reg_native(vm, nc, "matches(sel)", native_el_matches, bridge);
                vm_reg_native(vm, nc, "closest(sel)", native_el_closest, bridge);
            }
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

    /* customElements: github.com's <react-partial> and friends are SSR'd
     * custom elements whose content only appears once the registry upgrades
     * them; with the binding missing every bundle that registers one threw
     * "can not find function 'define'" and hydration never started.
     * vm_reg_static lands methods on var_get_prototype(cls), so the registry
     * singleton MUST carry a prototype object or the methods would fall to
     * root (a no-proto cls makes get_prototype return NULL) and the singleton
     * itself would read as an empty object to `customElements.define(...)`. */
    var_t* ce_proto = var_new_obj_no_proto(vm, NULL, NULL);
    var_t* ce = var_new_obj(vm, ce_proto, NULL, NULL);
    vm_reg_static(vm, ce, "define(n, c)", native_ce_define, bridge);
    vm_reg_static(vm, ce, "get(n)", native_ce_get, bridge);
    vm_reg_static(vm, ce, "whenDefined(n)", native_ce_whenDefined, bridge);
    vm_reg_static(vm, ce, "upgrade(el)", native_ce_upgrade, bridge);
    var_add(window, "customElements", ce);
    /* Bundles call `customElements.upgrade(...)` / `.define(...)` as a BARE
     * global; mario resolves bare names against vm->root, not against window,
     * so the registry has to live on both (same reason URLSearchParams does). */
    var_add(vm->root, "customElements", ce);

    /* Timers are global functions, registered on vm->root exactly like alert()
     * above, with `bridge` as the native `data` so state_any() recovers st. */
    vm_reg_static(vm, NULL, "setInterval(fn, ms)", native_setInterval,   bridge);
    vm_reg_static(vm, NULL, "setTimeout(fn, ms)",  native_setTimeout,    bridge);
    vm_reg_static(vm, NULL, "clearInterval(id)",   native_clearInterval, bridge);
    vm_reg_static(vm, NULL, "clearTimeout(id)",    native_clearTimeout,  bridge);
    vm_reg_static(vm, NULL, "requestIdleCallback(fn, opts)", native_requestIdleCallback, bridge);
    vm_reg_static(vm, NULL, "cancelIdleCallback(id)",        native_cancelIdleCallback,  bridge);
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
    return js_add_timer(vm, st, cb, ms, repeat, false, false);
}

/* Queue a microtask (promise reaction / queueMicrotask / process.nextTick). It
 * rides the same table but is flagged so js_dom_poll_timers drains it ahead of
 * every 0-ms macrotask, restoring the spec's microtask-before-macrotask order.
 * Returns a positive id, or 0 when cb is not a function / the table is full -
 * callers that must not lose the reaction (CLI, table-full) run it inline. */
int js_dom_add_microtask(vm_t* vm, var_t* cb) {
    js_dom_state* st = state_from_vm(vm);
    if(st == NULL || cb == NULL || !cb->is_func) return 0;
    return js_add_timer(vm, st, cb, 0, false, true, false);
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
