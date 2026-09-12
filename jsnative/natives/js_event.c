/*
 * js_event.c - DOM Event / EventTarget natives for the mario JavaScript VM.
 *
 * See js_event.h for the contract. Pure C; every DOM operation goes through
 * the callback table the embedder already registered with
 * js_register_dom_natives(), reached here via js_dom_ctx()/js_dom_callbacks().
 *
 * Object model:
 *   - A hidden bridge var hangs off vm->root under EVENT_BRIDGE_KEY carrying
 *     the js_event_state in its ->value: the listener table, the inline
 *     handler compile cache, and borrowed pointers to the `window` and
 *     `document` globals the DOM bridge created.
 *   - Listeners are plain C records, NOT JS objects, so matching during
 *     propagation is a table scan with no allocation. Every live callback is
 *     mirrored into a hidden anchor array on the bridge var, because the GC
 *     marks from vm->root and ignores refcounts (the same trick js_dom.c uses
 *     for timer callbacks).
 *   - Event instances are objects of class Event/CustomEvent/MouseEvent/
 *     KeyboardEvent whose members are ordinary data properties. The engine has
 *     no public prototype-chaining API, so the shared methods are registered
 *     on each of the four prototypes from one table.
 *
 * Propagation follows the DOM: capture (root -> target's parent), target, then
 * bubble (target's parent -> root) when `bubbles` is set. The chain always
 * ends at document and then window, so a listener on window sees everything.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "js_event.h"
#include "js_natives_priv.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define CLS_EVENT        "Event"
#define CLS_CUSTOMEVENT  "CustomEvent"
#define CLS_MOUSEEVENT   "MouseEvent"
#define CLS_KEYEVENT     "KeyboardEvent"
#define CLS_ELEMENT      "Element"
#define CLS_DOCUMENT     "Document"

#define EVENT_BRIDGE_KEY "@@event_bridge"
/* Hidden array on the bridge var mirroring every live listener callback and
 * every cached inline handler, so the GC keeps them reachable across runs. */
#define EVENT_ANCHOR_KEY "@@listeners"

/* Upper bound on simultaneously registered listeners. Fixed table: no
 * allocation on the (hot) input path, and 64 is generous for a page. */
#define JS_LISTENER_MAX 64
/* Inline `on*="src"` handlers are compiled on demand; the cache keeps the
 * common case (the same attribute firing repeatedly) off the compiler. */
#define JS_INLINE_CACHE 16
/* Longest capture/bubble chain we walk. Deeper ancestors are simply not
 * visited - real pages never nest that far. */
#define JS_PATH_MAX 24

/* Global the inline-handler compiler parks the freshly built function in
 * before we move it into the cache. Double-underscored to stay out of the
 * way of page scripts; it is deleted right after being read. */
#define INLINE_FN_GLOBAL "__marioInlineHandler"

/* Hidden Event members driving propagation. Underscored so they read as
 * private, and never enumerated by pages. */
#define EV_STOP    "_stopPropagation"
#define EV_STOPNOW "_stopImmediate"
#define EV_PATH    "_path"

/* ------------------------------------------------------------------ */
/* Event type table                                                   */
/*                                                                    */
/* One entry per on* property installed on Element/Document/window.    */
/* Names use the canonical JS spelling (event types are case-sensitive */
/* in the DOM); the property name is derived by prefixing "on" and     */
/* folding to ASCII lower case.                                        */
/* ------------------------------------------------------------------ */

static const char* kEventTypes[] = {
    /* mouse */
    "click", "dblclick", "mousedown", "mouseup", "mouseover", "mouseout",
    "mousemove", "mouseenter", "mouseleave", "contextmenu", "wheel",
    /* keyboard */
    "keydown", "keyup", "keypress",
    /* forms */
    "input", "change", "submit", "reset", "select", "focus", "blur",
    "focusin", "focusout", "invalid",
    /* drag and drop */
    "drag", "dragstart", "dragend", "dragover", "dragenter", "dragleave", "drop",
    /* touch */
    "touchstart", "touchmove", "touchend", "touchcancel",
    /* document / window lifecycle */
    "load", "unload", "beforeunload", "error", "abort", "resize", "scroll",
    "readystatechange", "DOMContentLoaded", "visibilitychange",
    "hashchange", "popstate", "message", "storage",
    /* media + css */
    "play", "pause", "ended", "timeupdate", "progress", "canplay",
    "animationstart", "animationend", "transitionend",
    /* clipboard + misc */
    "copy", "cut", "paste", "toggle", "show", "search", "pointerdown",
    "pointerup", "pointermove"
};
#define EVENT_TYPE_COUNT ((int)(sizeof(kEventTypes) / sizeof(kEventTypes[0])))

/* Types whose event object carries the MouseEvent coordinate/button fields. */
static const char* kMouseTypes[] = {
    "click", "dblclick", "mousedown", "mouseup", "mouseover", "mouseout",
    "mousemove", "mouseenter", "mouseleave", "contextmenu", "wheel",
    "pointerdown", "pointerup", "pointermove"
};
/* Types whose event object carries the KeyboardEvent key fields. */
static const char* kKeyTypes[] = { "keydown", "keyup", "keypress" };

static bool type_in(const char* const* table, int n, const char* t) {
    for(int i = 0; i < n; ++i)
        if(strcmp(table[i], t) == 0) return true;
    return false;
}
static bool is_mouse_type(const char* t) {
    return type_in(kMouseTypes, (int)(sizeof(kMouseTypes)/sizeof(kMouseTypes[0])), t);
}
static bool is_key_type(const char* t) {
    return type_in(kKeyTypes, (int)(sizeof(kKeyTypes)/sizeof(kKeyTypes[0])), t);
}

/* ------------------------------------------------------------------ */
/* Bridge state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int           kind;       /* JS_EVENT_ON_* */
    js_element_t  handle;     /* element handle; NULL for document/window */
    var_t*        target;     /* the var addEventListener was called on */
    char*         type;       /* mario_malloc'd, verbatim spelling */
    var_t*        fn;         /* anchored via EVENT_ANCHOR_KEY */
    bool          capture;
    bool          once;
    bool          from_prop;  /* installed through the on* property */
    bool          active;
} js_listener_t;

typedef struct {
    char*  src;               /* mario_malloc'd attribute body */
    var_t* fn;                /* compiled wrapper; anchored */
} js_inline_t;

typedef struct {
    vm_t*         vm;
    var_t*        document;   /* borrowed: rooted by the DOM bridge */
    var_t*        window;     /* borrowed: rooted by the DOM bridge */
    js_listener_t listeners[JS_LISTENER_MAX];
    js_inline_t   inline_cache[JS_INLINE_CACHE];
    int           inline_next;   /* round-robin eviction cursor */
} js_event_state;

static void event_state_free(void* p) {
    js_event_state* st = (js_event_state*)p;
    if(st == NULL) return;
    for(int i = 0; i < JS_LISTENER_MAX; ++i)
        if(st->listeners[i].type != NULL) mario_free(st->listeners[i].type);
    for(int i = 0; i < JS_INLINE_CACHE; ++i)
        if(st->inline_cache[i].src != NULL) mario_free(st->inline_cache[i].src);
    mario_free(st);
}

static var_t* event_bridge_var(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return NULL;
    return var_find_own_member_var(vm->root, EVENT_BRIDGE_KEY);
}

static js_event_state* ev_state(vm_t* vm) {
    var_t* bridge = event_bridge_var(vm);
    return (bridge != NULL) ? (js_event_state*)bridge->value : NULL;
}

/* Rebuild the anchor array from the live listener set plus the inline cache,
 * so nothing the GC cannot otherwise reach gets collected between runs. */
static void ev_reanchor(vm_t* vm, js_event_state* st) {
    var_t* bridge = event_bridge_var(vm);
    if(bridge == NULL) return;
    vm->gc.gc_defer++;
    var_t* fresh = var_new_array(vm);
    for(int i = 0; i < JS_LISTENER_MAX; ++i)
        if(st->listeners[i].active && st->listeners[i].fn != NULL)
            var_array_add(fresh, st->listeners[i].fn);
    for(int i = 0; i < JS_INLINE_CACHE; ++i)
        if(st->inline_cache[i].fn != NULL)
            var_array_add(fresh, st->inline_cache[i].fn);
    node_t* n = var_add(bridge, EVENT_ANCHOR_KEY, fresh);
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
    vm->gc.gc_defer--;
}

/* ------------------------------------------------------------------ */
/* Target classification                                              */
/* ------------------------------------------------------------------ */

static js_element_t element_of(vm_t* vm, var_t* v) {
    if(v == NULL || v->type != V_OBJECT || v->is_func || v->is_array) return NULL;
    var_t* cls = var_find_own_member_var(vm->root, CLS_ELEMENT);
    if(cls != NULL && !var_instanceof(v, cls)) return NULL;
    return (js_element_t)v->value;
}

/* Identify a JS object as one of the three EventTarget flavours. Returns -1
 * for anything else (a plain object, an Event, ...). */
static int ev_classify(vm_t* vm, js_event_state* st, var_t* v, js_element_t* out) {
    if(out != NULL) *out = NULL;
    if(v == NULL || st == NULL) return -1;
    if(st->window != NULL && v == st->window)     return JS_EVENT_ON_WINDOW;
    if(st->document != NULL && v == st->document) return JS_EVENT_ON_DOCUMENT;
    js_element_t h = element_of(vm, v);
    if(h != NULL) {
        if(out != NULL) *out = h;
        return JS_EVENT_ON_ELEMENT;
    }
    return -1;
}

/* The JS object for a propagation-chain node. Element wrappers come from the
 * DOM bridge's identity cache, so repeated wraps of one handle are the same
 * var and `e.target === e.currentTarget` holds at the target phase. */
static var_t* ev_node_var(vm_t* vm, js_event_state* st, int kind, js_element_t h) {
    switch(kind) {
        case JS_EVENT_ON_DOCUMENT: return st->document;
        case JS_EVENT_ON_WINDOW:   return st->window;
        default:                   return js_dom_wrap_element(vm, h);
    }
}

/* ------------------------------------------------------------------ */
/* Listener table                                                     */
/* ------------------------------------------------------------------ */

static bool listener_matches(const js_listener_t* r, int kind, js_element_t h,
                             var_t* target, const char* type) {
    if(!r->active || r->kind != kind) return false;
    if(kind == JS_EVENT_ON_ELEMENT) {
        if(r->handle != h) return false;
    }
    else if(r->target != target) return false;
    return strcmp(r->type, type) == 0;
}

static void listener_drop(js_listener_t* r) {
    r->active = false;
    r->fn = NULL;
    r->target = NULL;
    r->handle = NULL;
    if(r->type != NULL) { mario_free(r->type); r->type = NULL; }
}

static bool ev_add_listener(vm_t* vm, js_event_state* st, int kind, js_element_t h,
                            var_t* target, const char* type, var_t* fn,
                            bool capture, bool once, bool from_prop) {
    if(st == NULL || type == NULL || type[0] == 0 || fn == NULL || !fn->is_func)
        return false;

    /* An on* property holds at most one handler: assigning replaces the
     * previous one (but leaves addEventListener registrations alone). */
    if(from_prop) {
        for(int i = 0; i < JS_LISTENER_MAX; ++i) {
            js_listener_t* r = &st->listeners[i];
            if(r->from_prop && listener_matches(r, kind, h, target, type))
                listener_drop(r);
        }
    }
    else {
        /* DOM: re-registering the same (callback, capture) pair is a no-op. */
        for(int i = 0; i < JS_LISTENER_MAX; ++i) {
            js_listener_t* r = &st->listeners[i];
            if(!r->from_prop && listener_matches(r, kind, h, target, type) &&
               r->fn == fn && r->capture == capture)
                return true;
        }
    }

    char* tcopy = js_strdup(type);
    if(tcopy == NULL) return false;

    for(int i = 0; i < JS_LISTENER_MAX; ++i) {
        js_listener_t* r = &st->listeners[i];
        if(r->active) continue;
        r->kind      = kind;
        r->handle    = h;
        r->target    = target;
        r->type      = tcopy;
        r->fn        = fn;
        r->capture   = capture;
        r->once      = once;
        r->from_prop = from_prop;
        r->active    = true;
        ev_reanchor(vm, st);
        return true;
    }
    mario_free(tcopy);
    return false;   /* table full */
}

static bool ev_remove_listener(vm_t* vm, js_event_state* st, int kind, js_element_t h,
                               var_t* target, const char* type, var_t* fn, bool capture) {
    if(st == NULL || type == NULL) return false;
    bool removed = false;
    for(int i = 0; i < JS_LISTENER_MAX; ++i) {
        js_listener_t* r = &st->listeners[i];
        if(!listener_matches(r, kind, h, target, type)) continue;
        if(fn != NULL && r->fn != fn) continue;
        if(r->capture != capture) continue;
        listener_drop(r);
        removed = true;
    }
    if(removed) ev_reanchor(vm, st);
    return removed;
}

/* The handler installed through the on* property, or NULL. */
static var_t* ev_prop_handler(js_event_state* st, int kind, js_element_t h,
                              var_t* target, const char* type) {
    if(st == NULL) return NULL;
    for(int i = 0; i < JS_LISTENER_MAX; ++i) {
        js_listener_t* r = &st->listeners[i];
        if(r->from_prop && listener_matches(r, kind, h, target, type))
            return r->fn;
    }
    return NULL;
}

/* Drop every listener on one target. Called when the embedder tears a page
 * down so stale element handles cannot be invoked later. */
static void ev_clear_listeners(vm_t* vm, js_event_state* st) {
    if(st == NULL) return;
    for(int i = 0; i < JS_LISTENER_MAX; ++i)
        if(st->listeners[i].active) listener_drop(&st->listeners[i]);
    ev_reanchor(vm, st);
}

int js_event_listener_count(vm_t* vm) {
    js_event_state* st = ev_state(vm);
    if(st == NULL) return 0;
    int n = 0;
    for(int i = 0; i < JS_LISTENER_MAX; ++i)
        if(st->listeners[i].active) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Event object construction                                          */
/* ------------------------------------------------------------------ */

static void ev_set(vm_t* vm, var_t* ev, const char* name, var_t* v) {
    (void)vm;
    if(ev != NULL) var_add(ev, name, v);
}
static void ev_set_str(vm_t* vm, var_t* ev, const char* name, const char* s) {
    ev_set(vm, ev, name, var_new_str(vm, (s != NULL) ? s : ""));
}
static void ev_set_int(vm_t* vm, var_t* ev, const char* name, int i) {
    ev_set(vm, ev, name, var_new_int(vm, i));
}
static void ev_set_bool(vm_t* vm, var_t* ev, const char* name, bool b) {
    ev_set(vm, ev, name, var_new_bool(vm, b));
}

static bool ev_flag(var_t* ev, const char* name) {
    return js_truthy(get_obj(ev, name));
}

/* Read a boolean out of an init dictionary (`{bubbles:true}`), defaulting to
 * `def` when the key is absent. get_obj() does not create missing members, so
 * probing an options object is side-effect free. */
static bool ev_opt_bool(var_t* opts, const char* name, bool def) {
    if(opts == NULL || opts->type != V_OBJECT || opts->is_array) return def;
    var_t* v = get_obj(opts, name);
    if(v == NULL || v->type == V_UNDEF) return def;
    return js_truthy(v);
}

static var_t* ev_opt_var(var_t* opts, const char* name) {
    if(opts == NULL || opts->type != V_OBJECT || opts->is_array) return NULL;
    var_t* v = get_obj(opts, name);
    if(v == NULL || v->type == V_UNDEF) return NULL;
    return v;
}

static double ev_opt_num(var_t* opts, const char* name, double def) {
    if(opts == NULL || opts->type != V_OBJECT || opts->is_array) return def;
    var_t* v = get_obj(opts, name);
    if(v == NULL || v->type == V_UNDEF) return def;
    return js_num(v);
}

/* Fill in the members shared by every event flavour. */
static void ev_fill_common(vm_t* vm, var_t* ev, const char* type,
                           bool bubbles, bool cancelable, bool trusted) {
    ev_set_str(vm, ev, "type", type);
    ev_set_bool(vm, ev, "bubbles", bubbles);
    ev_set_bool(vm, ev, "cancelable", cancelable);
    ev_set_bool(vm, ev, "composed", false);
    ev_set_bool(vm, ev, "defaultPrevented", false);
    ev_set_bool(vm, ev, "isTrusted", trusted);
    ev_set_bool(vm, ev, "cancelBubble", false);
    ev_set(vm, ev, "returnValue", var_new_bool(vm, true));
    ev_set_int(vm, ev, "eventPhase", 0);   /* NONE until dispatch starts */
    /* performance.now()-style stamp: the DOM bridge's monotonic clock. */
    ev_set(vm, ev, "timeStamp", var_new_float64(vm, (double)js_dom_monotonic_ms()));
    ev_set(vm, ev, "target", var_new_null(vm));
    ev_set(vm, ev, "currentTarget", var_new_null(vm));
    ev_set(vm, ev, "srcElement", var_new_null(vm));
    ev_set(vm, ev, "relatedTarget", var_new_null(vm));
    ev_set_bool(vm, ev, EV_STOP, false);
    ev_set_bool(vm, ev, EV_STOPNOW, false);
}

static void ev_fill_mouse(vm_t* vm, var_t* ev, const js_event_init_t* in) {
    ev_set_int(vm, ev, "clientX", in->client_x);
    ev_set_int(vm, ev, "clientY", in->client_y);
    /* pageX/Y are document coordinates: the client position plus however far
     * the document is scrolled. offsetX/Y stay client-relative here - the
     * bridge has no notion of the target's own border box, and pages that
     * need exact offsets read getBoundingClientRect() instead. */
    ev_set_int(vm, ev, "pageX",   in->client_x + in->scroll_x);
    ev_set_int(vm, ev, "pageY",   in->client_y + in->scroll_y);
    ev_set_int(vm, ev, "x",       in->client_x);
    ev_set_int(vm, ev, "y",       in->client_y);
    ev_set_int(vm, ev, "offsetX", in->client_x);
    ev_set_int(vm, ev, "offsetY", in->client_y);
    ev_set_int(vm, ev, "screenX", in->screen_x);
    ev_set_int(vm, ev, "screenY", in->screen_y);
    ev_set_int(vm, ev, "button",  in->button);
    ev_set_int(vm, ev, "buttons", in->buttons);
    ev_set_int(vm, ev, "detail",  in->detail);
    ev_set_int(vm, ev, "movementX", 0);
    ev_set_int(vm, ev, "movementY", 0);
    ev_set_bool(vm, ev, "altKey",   (in->mods & JS_EVENT_MOD_ALT)   != 0);
    ev_set_bool(vm, ev, "ctrlKey",  (in->mods & JS_EVENT_MOD_CTRL)  != 0);
    ev_set_bool(vm, ev, "shiftKey", (in->mods & JS_EVENT_MOD_SHIFT) != 0);
    ev_set_bool(vm, ev, "metaKey",  (in->mods & JS_EVENT_MOD_META)  != 0);
}

static void ev_fill_key(vm_t* vm, var_t* ev, const js_event_init_t* in) {
    ev_set_str(vm, ev, "key",  (in->key  != NULL) ? in->key  : "");
    ev_set_str(vm, ev, "code", (in->code != NULL) ? in->code : "");
    ev_set_int(vm, ev, "keyCode",  in->key_code);
    ev_set_int(vm, ev, "charCode", in->key_code);
    ev_set_int(vm, ev, "which",    in->key_code);
    ev_set_int(vm, ev, "location", 0);
    ev_set_bool(vm, ev, "repeat", in->repeat);
    ev_set_bool(vm, ev, "altKey",   (in->mods & JS_EVENT_MOD_ALT)   != 0);
    ev_set_bool(vm, ev, "ctrlKey",  (in->mods & JS_EVENT_MOD_CTRL)  != 0);
    ev_set_bool(vm, ev, "shiftKey", (in->mods & JS_EVENT_MOD_SHIFT) != 0);
    ev_set_bool(vm, ev, "metaKey",  (in->mods & JS_EVENT_MOD_META)  != 0);
}

/* Which class a native (C-side) event should be built as. A CustomEvent wins
 * whenever a detail payload is supplied, so the embedder can pass structured
 * data through any event type. */
static const char* ev_pick_class(const js_event_init_t* in) {
    if(in->custom_detail != NULL) return CLS_CUSTOMEVENT;
    if(is_mouse_type(in->type))   return CLS_MOUSEEVENT;
    if(is_key_type(in->type))     return CLS_KEYEVENT;
    return CLS_EVENT;
}

static var_t* ev_new(vm_t* vm, const js_event_init_t* in) {
    const char* cls = ev_pick_class(in);
    var_t* ev = new_obj(vm, cls, 0);
    if(ev == NULL) return NULL;
    ev_fill_common(vm, ev, in->type, in->bubbles, in->cancelable, in->trusted);
    if(in->custom_detail != NULL)
        ev_set(vm, ev, "detail", in->custom_detail);
    /* Populate the pointer/key fields for the matching flavour, and also for
     * a plain Event whose type says mouse/key: pages routinely synthesise
     * `new Event('click')` and then read clientX off it. */
    if(strcmp(cls, CLS_MOUSEEVENT) == 0 || is_mouse_type(in->type))
        ev_fill_mouse(vm, ev, in);
    if(strcmp(cls, CLS_KEYEVENT) == 0 || is_key_type(in->type))
        ev_fill_key(vm, ev, in);
    return ev;
}

/* ------------------------------------------------------------------ */
/* Event methods                                                      */
/* ------------------------------------------------------------------ */

static var_t* native_ev_preventDefault(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* ev = js_this(env);
    if(ev == NULL) return NULL;
    /* Honour the DOM rule: only a cancelable event can be prevented. */
    if(ev_flag(ev, "cancelable")) {
        ev_set_bool(vm, ev, "defaultPrevented", true);
        ev_set_bool(vm, ev, "returnValue", false);
    }
    return NULL;
}

static var_t* native_ev_stopPropagation(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* ev = js_this(env);
    if(ev == NULL) return NULL;
    ev_set_bool(vm, ev, EV_STOP, true);
    ev_set_bool(vm, ev, "cancelBubble", true);
    return NULL;
}

static var_t* native_ev_stopImmediate(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* ev = js_this(env);
    if(ev == NULL) return NULL;
    ev_set_bool(vm, ev, EV_STOP, true);
    ev_set_bool(vm, ev, EV_STOPNOW, true);
    ev_set_bool(vm, ev, "cancelBubble", true);
    return NULL;
}

/* composedPath(): the propagation chain captured when dispatch started. */
static var_t* native_ev_composedPath(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* ev = js_this(env);
    var_t* path = (ev != NULL) ? get_obj(ev, EV_PATH) : NULL;
    if(path != NULL && path->is_array) return path;
    return var_new_array(vm);
}

/* initEvent(type, bubbles, cancelable): the legacy re-arm entry point. */
static var_t* native_ev_initEvent(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* ev = js_this(env);
    if(ev == NULL) return NULL;
    mstr_t* s = mstr_new("");
    const char* type = js_arg_cstr(env, 0, s);
    ev_set_str(vm, ev, "type", type);
    ev_set_bool(vm, ev, "bubbles", js_truthy(js_arg(env, 1)));
    ev_set_bool(vm, ev, "cancelable", js_truthy(js_arg(env, 2)));
    ev_set_bool(vm, ev, "defaultPrevented", false);
    ev_set_bool(vm, ev, EV_STOP, false);
    ev_set_bool(vm, ev, EV_STOPNOW, false);
    ev_set_int(vm, ev, "eventPhase", 0);
    mstr_free(s);
    return NULL;
}

/* initCustomEvent(type, bubbles, cancelable, detail) */
static var_t* native_ev_initCustomEvent(vm_t* vm, var_t* env, void* data) {
    (void)data;
    native_ev_initEvent(vm, env, data);
    var_t* ev = js_this(env);
    var_t* detail = js_arg(env, 3);
    ev_set(vm, ev, "detail", (detail != NULL) ? detail : var_new_null(vm));
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Script-side constructors: new Event(type[, init]) & friends        */
/* ------------------------------------------------------------------ */

/* Shared body of the four constructors. `cls_kind` selects which extra field
 * group to fill, independently of the class name so that
 * `new Event('click', {clientX:1})` still exposes clientX. */
static var_t* ev_ctor(vm_t* vm, var_t* env, int cls_kind) {
    var_t* obj = js_this(env);
    if(obj == NULL) return NULL;

    mstr_t* s = mstr_new("");
    const char* type = js_arg_cstr(env, 0, s);
    var_t* opts = js_arg(env, 1);

    js_event_init_t in;
    js_event_init(&in, type);
    in.bubbles    = ev_opt_bool(opts, "bubbles", false);
    in.cancelable = ev_opt_bool(opts, "cancelable", false);
    in.trusted    = false;              /* script-created events are untrusted */
    in.detail     = (int)ev_opt_num(opts, "detail", 0);

    in.client_x = (int)ev_opt_num(opts, "clientX", 0);
    in.client_y = (int)ev_opt_num(opts, "clientY", 0);
    in.screen_x = (int)ev_opt_num(opts, "screenX", in.client_x);
    in.screen_y = (int)ev_opt_num(opts, "screenY", in.client_y);
    in.button   = (int)ev_opt_num(opts, "button", 0);
    in.buttons  = (int)ev_opt_num(opts, "buttons", 0);

    var_t* kv = ev_opt_var(opts, "key");
    mstr_t* ks = mstr_new("");
    if(kv != NULL) in.key = js_cstr(kv, ks);
    var_t* cv = ev_opt_var(opts, "code");
    mstr_t* cs = mstr_new("");
    if(cv != NULL) in.code = js_cstr(cv, cs);
    in.key_code = (int)ev_opt_num(opts, "keyCode", 0);
    in.repeat   = ev_opt_bool(opts, "repeat", false);
    if(ev_opt_bool(opts, "altKey", false))   in.mods |= JS_EVENT_MOD_ALT;
    if(ev_opt_bool(opts, "ctrlKey", false))  in.mods |= JS_EVENT_MOD_CTRL;
    if(ev_opt_bool(opts, "shiftKey", false)) in.mods |= JS_EVENT_MOD_SHIFT;
    if(ev_opt_bool(opts, "metaKey", false))  in.mods |= JS_EVENT_MOD_META;

    ev_fill_common(vm, obj, type, in.bubbles, in.cancelable, false);
    bool mouse = (cls_kind == 2) || is_mouse_type(type);
    bool key   = (cls_kind == 3) || is_key_type(type);
    if(mouse) ev_fill_mouse(vm, obj, &in);
    if(key)   ev_fill_key(vm, obj, &in);
    if(cls_kind == 1) {
        var_t* detail = ev_opt_var(opts, "detail");
        ev_set(vm, obj, "detail", (detail != NULL) ? detail : var_new_null(vm));
    }

    mstr_free(cs);
    mstr_free(ks);
    mstr_free(s);
    return NULL;   /* new_obj keeps `this` when the ctor returns a non-object */
}

static var_t* native_event_ctor(vm_t* vm, var_t* env, void* data)      { (void)data; return ev_ctor(vm, env, 0); }
static var_t* native_custom_ctor(vm_t* vm, var_t* env, void* data)     { (void)data; return ev_ctor(vm, env, 1); }
static var_t* native_mouse_ctor(vm_t* vm, var_t* env, void* data)      { (void)data; return ev_ctor(vm, env, 2); }
static var_t* native_key_ctor(vm_t* vm, var_t* env, void* data)        { (void)data; return ev_ctor(vm, env, 3); }

/* ------------------------------------------------------------------ */
/* EventTarget methods                                                */
/* ------------------------------------------------------------------ */

/* addEventListener(type, fn[, capture | {capture, once}]) */
static var_t* native_addEventListener(vm_t* vm, var_t* env, void* data) {
    js_event_state* st = ev_state(vm);
    var_t* self = js_this(env);
    if(st == NULL || self == NULL) return NULL;

    mstr_t* s = mstr_new("");
    const char* type = js_arg_cstr(env, 0, s);
    var_t* fn = js_arg(env, 1);
    var_t* opt = js_arg(env, 2);

    bool capture = false, once = false;
    if(opt != NULL && opt->type == V_OBJECT && !opt->is_array) {
        capture = ev_opt_bool(opt, "capture", false);
        once    = ev_opt_bool(opt, "once", false);
    }
    else {
        capture = js_truthy(opt);
    }

    js_element_t h = NULL;
    int kind = ev_classify(vm, st, self, &h);
    if(kind >= 0 && fn != NULL && fn->is_func)
        ev_add_listener(vm, st, kind, h, self, type, fn, capture, once, false);

    mstr_free(s);
    return NULL;
}

/* removeEventListener(type, fn[, capture]) */
static var_t* native_removeEventListener(vm_t* vm, var_t* env, void* data) {
    js_event_state* st = ev_state(vm);
    var_t* self = js_this(env);
    if(st == NULL || self == NULL) return NULL;

    mstr_t* s = mstr_new("");
    const char* type = js_arg_cstr(env, 0, s);
    var_t* fn = js_arg(env, 1);
    var_t* opt = js_arg(env, 2);
    bool capture = (opt != NULL && opt->type == V_OBJECT && !opt->is_array)
                 ? ev_opt_bool(opt, "capture", false) : js_truthy(opt);

    js_element_t h = NULL;
    int kind = ev_classify(vm, st, self, &h);
    if(kind >= 0 && fn != NULL)
        ev_remove_listener(vm, st, kind, h, self, type, fn, capture);

    mstr_free(s);
    return NULL;
}

static bool ev_dispatch_on(vm_t* vm, js_event_state* st, int kind,
                           js_element_t handle, var_t* ev);

/* dispatchEvent(event) -> false when a listener called preventDefault(). */
static var_t* native_dispatchEvent(vm_t* vm, var_t* env, void* data) {
    js_event_state* st = ev_state(vm);
    var_t* self = js_this(env);
    var_t* ev = js_arg(env, 0);
    if(st == NULL || self == NULL || ev == NULL || ev->type != V_OBJECT)
        return var_new_bool(vm, true);

    js_element_t h = NULL;
    int kind = ev_classify(vm, st, self, &h);
    if(kind < 0) return var_new_bool(vm, true);

    /* Script-dispatched events are untrusted; keep whatever the event says
     * when it was built in C (the embedder sets isTrusted true). */
    bool ok = ev_dispatch_on(vm, st, kind, h, ev);
    return var_new_bool(vm, ok);
}

/* ------------------------------------------------------------------ */
/* on* handler properties                                             */
/*                                                                    */
/* `data` carries the index into kEventTypes, exactly like js_dom.c's  */
/* attribute-property tables; the bridge is recovered from the vm.     */
/* ------------------------------------------------------------------ */

static var_t* on_prop_get(vm_t* vm, var_t* env, void* data) {
    int idx = (int)(intptr_t)data;
    if(idx < 0 || idx >= EVENT_TYPE_COUNT) return var_new_null(vm);
    js_event_state* st = ev_state(vm);
    var_t* self = js_this(env);
    if(st == NULL || self == NULL) return var_new_null(vm);
    js_element_t h = NULL;
    int kind = ev_classify(vm, st, self, &h);
    if(kind < 0) return var_new_null(vm);
    var_t* fn = ev_prop_handler(st, kind, h, self, kEventTypes[idx]);
    return (fn != NULL) ? fn : var_new_null(vm);
}

static var_t* on_prop_set(vm_t* vm, var_t* env, void* data) {
    int idx = (int)(intptr_t)data;
    if(idx < 0 || idx >= EVENT_TYPE_COUNT) return NULL;
    js_event_state* st = ev_state(vm);
    var_t* self = js_this(env);
    if(st == NULL || self == NULL) return NULL;
    js_element_t h = NULL;
    int kind = ev_classify(vm, st, self, &h);
    if(kind < 0) return NULL;

    var_t* fn = js_arg(env, 0);
    const char* type = kEventTypes[idx];
    if(fn == NULL || !fn->is_func) {
        /* `el.onclick = null` clears ONLY the property handler; listeners
         * added through addEventListener() are a separate registration and
         * must survive. */
        for(int i = 0; i < JS_LISTENER_MAX; ++i) {
            js_listener_t* r = &st->listeners[i];
            if(r->from_prop && listener_matches(r, kind, h, self, type))
                listener_drop(r);
        }
        ev_reanchor(vm, st);
        return NULL;
    }
    ev_add_listener(vm, st, kind, h, self, type, fn, false, false, true);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Inline `on*="src"` attribute handlers                              */
/*                                                                    */
/* The engine has no eval() and no Function.prototype.call, so the     */
/* attribute body is compiled as a global function assignment and then */
/* invoked from C through call_m_func with `this` bound to the element. */
/* vm_load_run_native() saves and restores vm->pc, which is exactly how */
/* the engine's own do_include() nests compilation inside a run, so    */
/* this is safe even mid-dispatch.                                     */
/* ------------------------------------------------------------------ */

static var_t* ev_inline_compile(vm_t* vm, js_event_state* st, const char* src) {
    if(st == NULL || src == NULL || src[0] == 0) return NULL;

    for(int i = 0; i < JS_INLINE_CACHE; ++i)
        if(st->inline_cache[i].src != NULL && st->inline_cache[i].fn != NULL &&
           strcmp(st->inline_cache[i].src, src) == 0)
            return st->inline_cache[i].fn;

    mstr_t* code = mstr_new("");
    mstr_append(code, INLINE_FN_GLOBAL " = function(event){ ");
    mstr_append(code, src);
    /* A trailing semicolon keeps a body without one (e.g. onclick="f()")
     * from swallowing the closing brace. */
    mstr_append(code, "; };");
    bool ok = vm_load_run_native(vm, code->cstr);
    mstr_free(code);
    if(!ok) return NULL;

    var_t* fn = var_find_own_member_var(vm->root, INLINE_FN_GLOBAL);
    if(fn == NULL) {
        node_t* n = vm_find(vm, INLINE_FN_GLOBAL);
        if(n != NULL) fn = n->var;
    }
    if(fn == NULL || !fn->is_func) return NULL;

    /* Cache it: the global stays in place (the engine has no member-delete
     * API) but the anchor array is what keeps the function alive, so the
     * next compile simply overwrites the global. */
    int slot = st->inline_next;
    st->inline_next = (slot + 1) % JS_INLINE_CACHE;
    if(st->inline_cache[slot].src != NULL) mario_free(st->inline_cache[slot].src);
    st->inline_cache[slot].src = js_strdup(src);
    st->inline_cache[slot].fn  = fn;
    ev_reanchor(vm, st);
    return fn;
}

/* Run `<el on{type}="src">` if the element carries such an attribute. HTML
 * attribute names are ASCII-case-insensitive and the parser folds them to
 * lower case, so the lookup name is lowercased even though event types are
 * case-sensitive in script ("DOMContentLoaded" -> "ondomcontentloaded"). */
static void ev_run_inline(vm_t* vm, js_event_state* st, js_element_t h,
                          var_t* obj, const char* type, var_t* ev) {
    const js_dom_callbacks_t* dom = js_dom_callbacks(vm);
    void* ctx = js_dom_ctx(vm);
    if(dom == NULL || dom->el_get_attr == NULL || h == NULL) return;

    char attr[72];
    int written = snprintf(attr, sizeof(attr), "on%s", type);
    if(written <= 0 || written >= (int)sizeof(attr)) return;
    for(char* p = attr; *p != 0; ++p) *p = js_ascii_lower(*p);

    char* src = dom->el_get_attr(ctx, h, attr);
    if(src == NULL) return;
    if(src[0] != 0) {
        var_t* fn = ev_inline_compile(vm, st, src);
        if(fn != NULL) {
            var_t* args = var_new_array(vm);
            var_array_add(args, ev);
            var_t* ret = call_m_func(vm, obj, fn, args);
            var_unref(args);
            if(ret != NULL) var_unref(ret);   /* release call_m_func's owned ref */
        }
    }
    mario_free(src);
}

/* ------------------------------------------------------------------ */
/* Propagation                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int           kind;    /* JS_EVENT_ON_* */
    js_element_t  handle;
    var_t*        obj;     /* JS wrapper, materialised once per dispatch */
} ev_node_t;

/* target, ancestors, document, window - in that order (index 0 is the
 * target). Two slots are reserved for document/window so the element walk
 * can never push them out. */
static int ev_build_chain(vm_t* vm, js_event_state* st, int kind, js_element_t h,
                          ev_node_t* chain, int max) {
    int n = 0;
    int el_max = max - 2;
    if(el_max < 1) el_max = 1;

    chain[n].kind = kind; chain[n].handle = h; chain[n].obj = NULL; n++;

    if(kind == JS_EVENT_ON_ELEMENT && h != NULL && n < el_max) {
        const js_dom_callbacks_t* dom = js_dom_callbacks(vm);
        void* ctx = js_dom_ctx(vm);
        if(dom != NULL && dom->el_parent != NULL) {
            js_element_t p = h;
            while(n < el_max) {
                p = dom->el_parent(ctx, p);
                if(p == NULL || p == chain[n-1].handle) break;   /* parent-of-self guard */
                chain[n].kind = JS_EVENT_ON_ELEMENT;
                chain[n].handle = p;
                chain[n].obj = NULL;
                n++;
            }
        }
    }
    if(kind == JS_EVENT_ON_ELEMENT && st->document != NULL && n < max) {
        chain[n].kind = JS_EVENT_ON_DOCUMENT; chain[n].handle = NULL; chain[n].obj = NULL; n++;
    }
    if(kind != JS_EVENT_ON_WINDOW && st->window != NULL && n < max) {
        chain[n].kind = JS_EVENT_ON_WINDOW; chain[n].handle = NULL; chain[n].obj = NULL; n++;
    }
    return n;
}

/* Invoke the listeners registered on one chain node. `phase` is the DOM
 * eventPhase constant; `at_target` runs both capture and bubble listeners
 * (plus the inline attribute handler) and ignores the capture flag.
 * Returns false once propagation must stop. */
static bool ev_invoke_at(vm_t* vm, js_event_state* st, const ev_node_t* nd,
                         const char* type, var_t* ev, int phase,
                         bool want_capture, bool at_target) {
    if(st == NULL || nd == NULL || nd->obj == NULL) return true;

    ev_set_int(vm, ev, "eventPhase", phase);
    ev_set(vm, ev, "currentTarget", nd->obj);

    /* The compiled attribute handler is the target's first listener, matching
     * how browsers order an onclick="" against addEventListener('click'). */
    if(at_target && nd->kind == JS_EVENT_ON_ELEMENT)
        ev_run_inline(vm, st, nd->handle, nd->obj, type, ev);
    if(ev_flag(ev, EV_STOP)) return false;

    for(int i = 0; i < JS_LISTENER_MAX; ++i) {
        js_listener_t* r = &st->listeners[i];
        if(!r->active) continue;
        if(!listener_matches(r, nd->kind, nd->handle, nd->obj, type)) continue;
        if(!at_target && r->capture != want_capture) continue;
        /* stopImmediatePropagation() must also silence the listeners still
         * queued on THIS node - that is the only thing separating it from
         * stopPropagation(). Tested per candidate rather than once per node,
         * so a listener registered earlier in the table can suppress one
         * registered later. */
        if(ev_flag(ev, EV_STOPNOW)) return false;

        var_t* fn = r->fn;
        bool once = r->once;

        var_t* args = var_new_array(vm);
        var_array_add(args, ev);
        var_t* ret = call_m_func(vm, nd->obj, fn, args);
        var_unref(args);
        if(ret != NULL) var_unref(ret);

        if(once) { listener_drop(r); ev_reanchor(vm, st); }
        if(ev_flag(ev, EV_STOP)) return false;
    }
    return true;
}

static bool ev_dispatch_on(vm_t* vm, js_event_state* st, int kind,
                           js_element_t handle, var_t* ev) {
    if(vm == NULL || st == NULL || ev == NULL || ev->type != V_OBJECT) return true;

    /* Copy the type out: the string lives inside `ev`, which we mutate below. */
    char type[72];
    const char* tv = get_str(ev, "type");
    if(tv == NULL || tv[0] == 0) return true;
    snprintf(type, sizeof(type), "%s", tv);

    var_ref(ev);          /* hold the event across the whole propagation */

    ev_node_t chain[JS_PATH_MAX];
    int n = ev_build_chain(vm, st, kind, handle, chain, JS_PATH_MAX);

    /* Materialise every chain node once and root them by hanging the array
     * off the event: this is composedPath()'s answer AND it keeps the element
     * wrappers reachable even if the DOM bridge's identity cache evicts one
     * while a listener runs. */
    var_t* path = var_new_array(vm);
    for(int i = 0; i < n; ++i) {
        chain[i].obj = ev_node_var(vm, st, chain[i].kind, chain[i].handle);
        var_array_add(path, (chain[i].obj != NULL) ? chain[i].obj : var_new_null(vm));
    }
    ev_set(vm, ev, EV_PATH, path);

    if(n > 0 && chain[0].obj != NULL) {
        ev_set(vm, ev, "target", chain[0].obj);
        ev_set(vm, ev, "srcElement", chain[0].obj);
    }
    /* Re-arm the control flags: an event object may be dispatched again. */
    ev_set_bool(vm, ev, EV_STOP, false);
    ev_set_bool(vm, ev, EV_STOPNOW, false);
    ev_set_bool(vm, ev, "defaultPrevented", false);
    ev_set_bool(vm, ev, "returnValue", true);

    bool bubbles = ev_flag(ev, "bubbles");

    /* 1. capturing: window -> ... -> target's parent */
    for(int i = n - 1; i >= 1; --i) {
        if(ev_flag(ev, EV_STOP)) break;
        ev_invoke_at(vm, st, &chain[i], type, ev, 1, true, false);
    }
    /* 2. at target */
    if(n > 0 && !ev_flag(ev, EV_STOP))
        ev_invoke_at(vm, st, &chain[0], type, ev, 2, false, true);
    /* 3. bubbling: target's parent -> window (capture phase always runs,
     *    this one only for bubbling events) */
    if(bubbles) {
        for(int i = 1; i < n; ++i) {
            if(ev_flag(ev, EV_STOP)) break;
            ev_invoke_at(vm, st, &chain[i], type, ev, 3, false, false);
        }
    }

    ev_set_int(vm, ev, "eventPhase", 0);
    ev_set(vm, ev, "currentTarget", var_new_null(vm));

    bool cancelled = ev_flag(ev, "defaultPrevented");
    var_unref(ev);
    return !cancelled;
}

/* ------------------------------------------------------------------ */
/* Public dispatch API                                                */
/* ------------------------------------------------------------------ */

void js_event_init(js_event_init_t* init, const char* type) {
    if(init == NULL) return;
    memset(init, 0, sizeof(*init));
    init->type = type;
    init->on = JS_EVENT_ON_ELEMENT;
}

bool js_event_dispatch(vm_t* vm, const js_event_init_t* init) {
    js_event_state* st = ev_state(vm);
    if(st == NULL || init == NULL || init->type == NULL || init->type[0] == 0)
        return true;

    int kind = init->on;
    js_element_t h = init->target;
    if(kind == JS_EVENT_ON_ELEMENT) {
        if(h == NULL) return true;         /* nothing to aim at */
    }
    else {
        h = NULL;
    }

    var_t* ev = ev_new(vm, init);
    if(ev == NULL) return true;
    /* ev_dispatch_on refs then unrefs, which releases ev_new's ownership. */
    return ev_dispatch_on(vm, st, kind, h, ev);
}

bool js_event_dispatch_simple(vm_t* vm, js_element_t el, const char* type, bool bubbles) {
    js_event_init_t in;
    js_event_init(&in, type);
    in.target = el;
    in.bubbles = bubbles;
    in.cancelable = false;
    in.trusted = true;
    in.detail = 0;
    return js_event_dispatch(vm, &in);
}

bool js_event_dispatch_mouse(vm_t* vm, js_element_t el, const char* type,
                             int x, int y, int scroll_x, int scroll_y,
                             int button, int click_count) {
    js_event_init_t in;
    js_event_init(&in, type);
    in.target = el;
    in.bubbles = true;
    /* mousedown/mouseup/click are the cancelable ones (preventDefault on click
     * is how a page suppresses navigation). */
    in.cancelable = (strcmp(type, "click") == 0 || strcmp(type, "mousedown") == 0 ||
                     strcmp(type, "mouseup") == 0 || strcmp(type, "dblclick") == 0 ||
                     strcmp(type, "contextmenu") == 0);
    in.trusted = true;
    in.client_x = x;
    in.client_y = y;
    in.screen_x = x;
    in.screen_y = y;
    in.scroll_x = scroll_x;
    in.scroll_y = scroll_y;
    in.button = button;
    in.buttons = (button == 0) ? 1 : (button == 2 ? 2 : 4);
    in.detail = click_count;
    return js_event_dispatch(vm, &in);
}

bool js_event_dispatch_key(vm_t* vm, js_element_t el, const char* type,
                           const char* key, int key_code, unsigned mods) {
    js_event_init_t in;
    js_event_init(&in, type);
    in.on = (el != NULL) ? JS_EVENT_ON_ELEMENT : JS_EVENT_ON_DOCUMENT;
    in.target = el;
    in.bubbles = true;
    in.cancelable = true;
    in.trusted = true;
    in.key = key;
    in.key_code = key_code;
    in.mods = mods;
    return js_event_dispatch(vm, &in);
}

bool js_event_dispatch_custom(vm_t* vm, js_element_t el, const char* type,
                              var_t* detail, bool bubbles) {
    js_event_init_t in;
    js_event_init(&in, type);
    in.on = (el != NULL) ? JS_EVENT_ON_ELEMENT : JS_EVENT_ON_WINDOW;
    in.target = el;
    in.bubbles = bubbles;
    in.trusted = false;
    in.custom_detail = detail;
    return js_event_dispatch(vm, &in);
}

/* Dispatch on one of the two globals (no element handle involved). */
static void ev_fire_global(vm_t* vm, int kind, const char* type, bool bubbles) {
    js_event_init_t in;
    js_event_init(&in, type);
    in.on = kind;
    in.target = NULL;
    in.bubbles = bubbles;
    in.trusted = true;
    js_event_dispatch(vm, &in);
}

void js_event_fire_dom_content_loaded(vm_t* vm) {
    ev_fire_global(vm, JS_EVENT_ON_DOCUMENT, "DOMContentLoaded", true);
}

void js_event_fire_load(vm_t* vm) {
    ev_fire_global(vm, JS_EVENT_ON_DOCUMENT, "load", false);
    ev_fire_global(vm, JS_EVENT_ON_WINDOW, "load", false);
    /* HTML puts <body onload> on the window's load slot, so it has to run as
     * part of the same lifecycle step. */
    js_event_fire_body_load(vm);
}

void js_event_fire_body_load(vm_t* vm) {
    const js_dom_callbacks_t* dom = js_dom_callbacks(vm);
    void* ctx = js_dom_ctx(vm);
    if(dom == NULL) return;
    js_element_t body = NULL;
    if(dom->get_body != NULL) body = dom->get_body(ctx);
    if(body == NULL && dom->get_element_by_id != NULL)
        body = dom->get_element_by_id(ctx, "body");
    if(body == NULL && dom->get_root != NULL) body = dom->get_root(ctx);
    if(body == NULL) return;
    js_event_dispatch_simple(vm, body, "load", false);
}

void js_event_fire_resize(vm_t* vm, int width, int height) {
    (void)width; (void)height;   /* window.innerWidth & co. come from js_web */
    ev_fire_global(vm, JS_EVENT_ON_WINDOW, "resize", false);
}

void js_event_fire_scroll(vm_t* vm) {
    ev_fire_global(vm, JS_EVENT_ON_DOCUMENT, "scroll", true);
}

void js_event_fire_unload(vm_t* vm) {
    const js_dom_callbacks_t* dom = js_dom_callbacks(vm);
    void* ctx = js_dom_ctx(vm);
    if(dom != NULL && dom->get_body != NULL) {
        js_element_t body = dom->get_body(ctx);
        if(body != NULL) js_event_dispatch_simple(vm, body, "unload", false);
    }
    ev_fire_global(vm, JS_EVENT_ON_WINDOW, "unload", false);
}

void js_event_fire_error(vm_t* vm, const char* message) {
    js_event_init_t in;
    js_event_init(&in, "error");
    in.on = JS_EVENT_ON_WINDOW;
    in.trusted = true;
    /* window.onerror's signature predates addEventListener, so the message
     * travels as a plain member too. */
    js_event_state* st = ev_state(vm);
    if(st == NULL) return;
    var_t* ev = ev_new(vm, &in);
    if(ev == NULL) return;
    var_ref(ev);
    ev_set_str(vm, ev, "message", (message != NULL) ? message : "");
    bool ok = ev_dispatch_on(vm, st, JS_EVENT_ON_WINDOW, NULL, ev);
    var_unref(ev);
    if(!ok) { /* the page handled it; nothing else to do */ }
}

void js_event_clear_listeners(vm_t* vm) {
    ev_clear_listeners(vm, ev_state(vm));
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

bool js_register_event_natives(vm_t* vm) {
    if(vm == NULL) return false;
    /* The DOM bridge owns the embedder context, the Element class and the
     * window/document globals; without it there is nothing to attach to. */
    if(js_dom_callbacks(vm) == NULL) return false;

    js_event_state* st = (js_event_state*)mario_malloc(sizeof(js_event_state));
    if(st == NULL) return false;
    memset(st, 0, sizeof(*st));
    st->vm       = vm;
    st->document = var_find_own_member_var(vm->root, "document");
    st->window   = var_find_own_member_var(vm->root, "window");

    var_t* bridge = var_new_obj_no_proto(vm, st, event_state_free);
    if(bridge == NULL) {
        mario_free(st);
        return false;
    }
    var_add(vm->root, EVENT_BRIDGE_KEY, bridge);

    /* ---- Event classes ----
     * mario exposes no prototype-chaining API (var_set_father is static), so
     * CustomEvent/MouseEvent/KeyboardEvent are independent classes that share
     * the same method set, registered from this one table. */
    static const char* kEventClasses[] = {
        CLS_EVENT, CLS_CUSTOMEVENT, CLS_MOUSEEVENT, CLS_KEYEVENT
    };
    var_t* ev_cls[4];
    for(int c = 0; c < 4; ++c) {
        ev_cls[c] = vm_new_class(vm, kEventClasses[c]);
        if(ev_cls[c] == NULL) continue;
        vm_reg_native(vm, ev_cls[c], "preventDefault()", native_ev_preventDefault, bridge);
        vm_reg_native(vm, ev_cls[c], "stopPropagation()", native_ev_stopPropagation, bridge);
        vm_reg_native(vm, ev_cls[c], "stopImmediatePropagation()", native_ev_stopImmediate, bridge);
        vm_reg_native(vm, ev_cls[c], "composedPath()", native_ev_composedPath, bridge);
        vm_reg_native(vm, ev_cls[c], "initEvent(t, b, c)", native_ev_initEvent, bridge);
        vm_reg_native(vm, ev_cls[c], "initCustomEvent(t, b, c, d)", native_ev_initCustomEvent, bridge);
        /* eventPhase constants resolve both as Event.NONE and ev.NONE, since
         * vm_reg_var installs them on the prototype. */
        vm_reg_var(vm, ev_cls[c], "NONE",            var_new_int(vm, 0), true);
        vm_reg_var(vm, ev_cls[c], "CAPTURING_PHASE", var_new_int(vm, 1), true);
        vm_reg_var(vm, ev_cls[c], "AT_TARGET",       var_new_int(vm, 2), true);
        vm_reg_var(vm, ev_cls[c], "BUBBLING_PHASE",  var_new_int(vm, 3), true);
    }
    /* Script-side constructors: `new Event(type, init)` & friends. */
    if(ev_cls[0] != NULL)
        vm_reg_native(vm, ev_cls[0], "constructor(type, options)", native_event_ctor, bridge);
    if(ev_cls[1] != NULL)
        vm_reg_native(vm, ev_cls[1], "constructor(type, options)", native_custom_ctor, bridge);
    if(ev_cls[2] != NULL)
        vm_reg_native(vm, ev_cls[2], "constructor(type, options)", native_mouse_ctor, bridge);
    if(ev_cls[3] != NULL)
        vm_reg_native(vm, ev_cls[3], "constructor(type, options)", native_key_ctor, bridge);

    /* ---- EventTarget methods ----
     * Document and Element are classes, so the methods go on their prototypes;
     * `window` is a plain object created by the DOM bridge and takes them
     * directly. */
    var_t* doc_cls = var_find_own_member_var(vm->root, CLS_DOCUMENT);
    var_t* el_cls  = var_find_own_member_var(vm->root, CLS_ELEMENT);

    if(doc_cls != NULL) {
        vm_reg_native(vm, doc_cls, "addEventListener(t, f, o)",    native_addEventListener, bridge);
        vm_reg_native(vm, doc_cls, "removeEventListener(t, f, o)", native_removeEventListener, bridge);
        vm_reg_native(vm, doc_cls, "dispatchEvent(e)",             native_dispatchEvent, bridge);
    }
    if(el_cls != NULL) {
        vm_reg_native(vm, el_cls, "addEventListener(t, f, o)",    native_addEventListener, bridge);
        vm_reg_native(vm, el_cls, "removeEventListener(t, f, o)", native_removeEventListener, bridge);
        vm_reg_native(vm, el_cls, "dispatchEvent(e)",             native_dispatchEvent, bridge);
    }
    if(st->window != NULL) {
        vm_reg_native_on(vm, st->window, "addEventListener(t, f, o)",    native_addEventListener, bridge);
        vm_reg_native_on(vm, st->window, "removeEventListener(t, f, o)", native_removeEventListener, bridge);
        vm_reg_native_on(vm, st->window, "dispatchEvent(e)",             native_dispatchEvent, bridge);
    }

    /* ---- on* handler properties ----
     * One accessor pair per event type, on all three targets. The row index
     * travels through the native's `data` pointer (js_dom.c uses the same
     * trick for its attribute tables). */
    var_t* doc_proto = (doc_cls != NULL) ? var_get_prototype(doc_cls) : NULL;
    var_t* el_proto  = (el_cls  != NULL) ? var_get_prototype(el_cls)  : NULL;

    char prop[72];
    for(int i = 0; i < EVENT_TYPE_COUNT; ++i) {
        int w = snprintf(prop, sizeof(prop), "on");
        if(w < 0 || w >= (int)sizeof(prop)) continue;
        const char* t = kEventTypes[i];
        size_t room = sizeof(prop) - (size_t)w - 1;
        size_t k = 0;
        for(; t[k] != 0 && k < room; ++k)
            prop[w + (int)k] = js_ascii_lower(t[k]);
        prop[w + (int)k] = 0;

        void* idx = (void*)(intptr_t)i;
        if(el_proto  != NULL) js_acc_on(vm, el_proto,  prop, on_prop_get, on_prop_set, idx);
        if(doc_proto != NULL) js_acc_on(vm, doc_proto, prop, on_prop_get, on_prop_set, idx);
        if(st->window != NULL) js_acc_on(vm, st->window, prop, on_prop_get, on_prop_set, idx);
    }

    return true;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */
