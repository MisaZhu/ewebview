/*
 * js_dom.h - Browser DOM natives for the mario JavaScript VM.
 *
 * This is a pure C bridge: it knows nothing about WidgetWebview, litehtml,
 * or EwokOS. The embedding application supplies a js_dom_callbacks_t whose
 * function pointers do the real DOM work; the natives in this file translate
 * JavaScript calls into those callbacks.
 *
 * Lifetime / threading contract:
 *   - js_register_dom_natives() must be called after vm_init() and before
 *     any script runs. The `ctx` pointer is opaque and passed verbatim to
 *     every callback.
 *   - All callbacks run synchronously on the VM thread (vm_run). The
 *     embedder is responsible for any locking needed to touch the DOM.
 *   - Element handles returned by get_element_by_id()/query_all()/el_child()
 *     are opaque pointers valid for the duration of the current page (they
 *     are invalidated by a re-parse). The natives never cache them across
 *     script runs.
 *   - char* returns are malloc'd with mario_malloc(); the natives free them
 *     after copying into a JS string. Callbacks must NOT return string
 *     literals or stack buffers.
 *   - Every callback is OPTIONAL. A NULL callback makes the corresponding
 *     JS API degrade the way it would on a missing feature (null / "" / 0 /
 *     false) instead of crashing, so an embedder can adopt the bridge
 *     incrementally.
 *
 * Supported surface (ES5-flavored, mirrors the browser classics):
 *   console.log(...)                  - provided by mario builtins
 *   alert(msg)
 *   document.write(html) / writeln(html)
 *   document.title                    - get/set
 *   document.body / head / documentElement
 *   document.URL / documentURI / readyState / domain / referrer
 *   document.getElementById(id)
 *   document.querySelector(sel) / querySelectorAll(sel)
 *   document.getElementsByTagName(tag) / getElementsByClassName(cls)
 *   document.getElementsByName(name)
 *   document.createElement(tag) / createTextNode(text)
 *   Element.textContent / innerText / innerHTML   - get/set
 *   Element.id / src / href / alt / type / name / value / title ...  - get/set
 *   Element.tagName / nodeName / className          - get (className set)
 *   Element.getAttribute / setAttribute / hasAttribute / removeAttribute
 *   Element.classList.{add,remove,toggle,contains,item,replace,length,value}
 *   Element.style.{cssText,getPropertyValue,setProperty,removeProperty}
 *          plus the common camelCase properties (style.color, style.display..)
 *   Element.children / childNodes / childElementCount / firstChild / lastChild
 *           firstElementChild / lastElementChild / nextSibling / previousSibling
 *           parentNode / parentElement
 *   Element.appendChild / insertBefore / replaceChild / removeChild / remove
 *           contains / querySelector(All) / getElementsByTagName / matches
 *           closest / index
 *   Element.value / checked / disabled / readOnly / selected / hidden
 *           placeholder / htmlFor / width / height / rel / action / method
 *   Element.offsetWidth / offsetHeight / offsetLeft / offsetTop
 *           clientWidth / clientHeight / getBoundingClientRect()
 *           focus() / blur() / scrollIntoView()
 *   window.location.href              - get (the rest lives in js_web.h)
 *   setInterval / setTimeout / clearInterval / clearTimeout
 *   requestAnimationFrame / cancelAnimationFrame
 */

#ifndef MARIO_JS_DOM_H
#define MARIO_JS_DOM_H

#include "mario.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque element handle. The embedder decides what this points at (e.g. a
 * litehtml::element*); the natives only pass it back into el_* callbacks. */
typedef void* js_element_t;

/* Maximum number of elements one query_all() call can hand back. The natives
 * page through the callback in chunks of this size, so a huge result set is
 * still complete - the constant only bounds the stack buffer. */
#define JS_DOM_QUERY_CHUNK 32

typedef struct js_dom_callbacks {
    /* alert(msg) - typically routes to a status bar or modal. */
    void (*alert)(void* ctx, const char* msg);

    /* document.write(html) / document.writeln(html).
     * The natives accumulate writes into an internal buffer; the embedder
     * drains it via js_dom_take_write_buffer() after vm_run() returns.
     * This callback is OPTIONAL: if NULL, writes are still buffered and
     * can be retrieved; if non-NULL it is invoked for each write so the
     * embedder can react immediately (e.g. live status). */
    void (*document_write)(void* ctx, const char* html);

    /* document.title get/set. get_title returns a mario_malloc'd string
     * (or NULL); set_title receives a NUL-terminated UTF-8 string. */
    char* (*get_title)(void* ctx);
    void  (*set_title)(void* ctx, const char* title);

    /* window.location.href - mario_malloc'd string or NULL. */
    char* (*get_url)(void* ctx);

    /* document.getElementById(id) -> opaque handle, or NULL if not found. */
    js_element_t (*get_element_by_id)(void* ctx, const char* id);

    /* Element property/method callbacks. `el` is a handle previously handed
     * out by this bridge. All char* returns are mario_malloc'd (or NULL).
     * Setters receive NUL-terminated strings.
     *
     * The natives call these while the VM is running; the embedder must
     * ensure the handle is still valid (e.g. by holding a DOM lock). */
    char* (*el_get_text)(void* ctx, js_element_t el);
    void  (*el_set_text)(void* ctx, js_element_t el, const char* text);
    char* (*el_get_html)(void* ctx, js_element_t el);
    void  (*el_set_html)(void* ctx, js_element_t el, const char* html);
    char* (*el_get_attr)(void* ctx, js_element_t el, const char* name);
    void  (*el_set_attr)(void* ctx, js_element_t el, const char* name, const char* value);
    char* (*el_get_tag)(void* ctx, js_element_t el);

    /* Handle-liveness gate. A JS Element wrapper can outlive the node it wraps:
     * the embedder frees the element (innerHTML replacement, a document
     * re-parse/swap, subtree removal) while the script still holds the wrapper,
     * leaving ->value a dangling pointer that the VM may even recycle for its
     * own objects. element_arg()/handle_from_this() call this before returning a
     * handle, so every native degrades to null/""/false instead of dereferencing
     * freed memory and corrupting the tree. OPTIONAL: NULL means "trust the
     * handle" (the historical behaviour). Must be cheap and must not dereference
     * anything beyond `el`'s own liveness tag. */
    bool (*el_is_live)(void* ctx, js_element_t el);

    /* Element.id is served by el_get_attr(ctx, el, "id"), so no dedicated
     * callback is needed - the embedder already exposes attributes.
     *
     * el_get_tag may return the tag name in any case (HTML parsers usually
     * fold it to lower case); the bridge upper-cases it before exposing it as
     * Element.tagName / Node.nodeName, which is what the DOM specifies for
     * HTML elements. */

    /* ---- document structure ---- */

    /* <html>, <body>, <head>. get_body falls back to
     * get_element_by_id(ctx, "body") when NULL, preserving the historical
     * single-callback behaviour. */
    js_element_t (*get_root)(void* ctx);
    js_element_t (*get_body)(void* ctx);
    js_element_t (*get_head)(void* ctx);
    /* document.currentScript: the <script> element whose body is executing
     * right now, or NULL outside a script run. SDKs use it to insert their
     * loader next to themselves. OPTIONAL: NULL reports null. */
    js_element_t (*get_current_script)(void* ctx);

    /* CSS selector query - backs querySelector/querySelectorAll on both
     * Document and Element. `root` is NULL for a document-wide query,
     * otherwise the element whose subtree is searched (root itself excluded,
     * matching the DOM). Fills out[0..max-1] in document order and returns
     * how many were written (0..max); returning max signals "there may be
     * more", so the natives call again with `skip` raised.
     *
     * NOTE: `skip` is the number of matches to ignore before filling `out`,
     * which lets an embedder that cannot cheaply count matches still page
     * through a large result set. */
    int (*query_all)(void* ctx, js_element_t root, const char* selector,
                     int skip, js_element_t* out, int max);

    /* document.createElement(tag) / createTextNode(text). The returned node
     * is detached: it becomes part of the document only once appended. Both
     * may return NULL when the embedder cannot build the node. */
    js_element_t (*create_element)(void* ctx, const char* tag);
    js_element_t (*create_text_node)(void* ctx, const char* text);
    /* document.createComment(text): a nodeType-8 marker node (React Suspense
     * boundaries). Optional; NULL makes createComment throw. */
    js_element_t (*create_comment)(void* ctx, const char* text);

    /* ---- tree walking / mutation ---- */

    js_element_t (*el_parent)(void* ctx, js_element_t el);
    int          (*el_child_count)(void* ctx, js_element_t el);
    js_element_t (*el_child)(void* ctx, js_element_t el, int idx);
    /* true for real elements, false for text nodes. Used to tell `children`
     * (elements only) from `childNodes` (everything). NULL => every child is
     * treated as an element. */
    bool         (*el_is_tag)(void* ctx, js_element_t el);
    /* True for comment nodes (nodeType 8); NULL means "no comments exist". */
    bool         (*el_is_comment)(void* ctx, js_element_t el);

    /* All three return true on success. insert_before appends when `ref` is
     * NULL. The embedder owns layout invalidation for these mutations. */
    bool (*el_append_child)(void* ctx, js_element_t parent, js_element_t child);
    bool (*el_insert_before)(void* ctx, js_element_t parent, js_element_t child, js_element_t ref);
    bool (*el_remove_child)(void* ctx, js_element_t parent, js_element_t child);
    /* cloneNode(deep): returns a freshly built, detached copy of `el` (deep:
     * children too), or NULL. The embedder styles and lays it out when it is
     * inserted, exactly like a create_element node. */
    js_element_t (*el_clone_node)(void* ctx, js_element_t el, int deep);

    void (*el_remove_attr)(void* ctx, js_element_t el, const char* name);

    /* ---- geometry / computed style / focus ---- */

    /* Border box in document coordinates (the same space the embedder
     * renders in), backing getBoundingClientRect() and the offset/client
     * size properties. Any of the out pointers may be NULL. */
    void  (*el_get_rect)(void* ctx, js_element_t el, int* x, int* y, int* w, int* h);
    /* Resolved (computed) value of a CSS property, mario_malloc'd or NULL. */
    char* (*el_get_style)(void* ctx, js_element_t el, const char* prop);
    void  (*el_focus)(void* ctx, js_element_t el);
    /* Drop keyboard focus from `el` when it currently holds it (Element.blur()).
     * OPTIONAL: without it blur() is a no-op. */
    void  (*el_blur)(void* ctx, js_element_t el);
    /* document.activeElement: the focused element, or <body> when nothing is
     * focused. OPTIONAL: without it activeElement reports <body>. */
    js_element_t (*get_active_element)(void* ctx);
    /* Live selection of a text form control (input/textarea) in codepoint
     * offsets. el_get_sel returns false when the element carries no editable
     * selection, so selectionStart/End report 0 and setSelectionRange()/
     * select() no-op. OPTIONAL. */
    bool (*el_get_sel)(void* ctx, js_element_t el, int* s, int* e);
    void (*el_set_sel)(void* ctx, js_element_t el, int s, int e);
    /* Snapshot of the element's data-* attributes as camelCase key/value
     * pairs packed "key\x1fvalue\x1e...", malloc'd with mario_malloc (the
     * bridge frees with mario_free). NULL when the element has none. Backs
     * Element.dataset. OPTIONAL. */
    char* (*el_get_dataset)(void* ctx, js_element_t el);
    /* Snapshot of ALL attributes as "name\x1fvalue\x1e..." pairs, malloc'd
     * with mario_malloc (the bridge frees with mario_free). Backs
     * Element.attributes (NamedNodeMap: length + indexed {name, value}
     * nodes); w3.org's nav enhancement copies every non-href attribute of
     * the top-level link onto the button it creates. OPTIONAL. */
    char* (*el_attr_snapshot)(void* ctx, js_element_t el);
    void  (*el_scroll_into_view)(void* ctx, js_element_t el);
} js_dom_callbacks_t;

/* Register `document`, `window`, `alert`, the timers and the Element class
 * on `vm`. `ctx` is passed verbatim to every callback. `cb` is copied by
 * value, so the caller may free its copy after this returns.
 *
 * Must be called AFTER vm_init() (which installs the builtin natives
 * including Console) and BEFORE vm_load()/vm_run().
 *
 * Returns true on success, false if vm/cb are NULL or registration failed. */
bool js_register_dom_natives(vm_t* vm, void* ctx, const js_dom_callbacks_t* cb);

/* Drain the document.write buffer accumulated since the last call (or
 * since js_register_dom_natives). Returns a mario_malloc'd NUL-terminated
 * string (possibly empty, never NULL) that the caller must mario_free().
 *
 * Typical use: after vm_run() returns, call this; if the result is
 * non-empty, splice it into the HTML and re-parse the document. */
char* js_dom_take_write_buffer(vm_t* vm);

/* Drive setInterval()/setTimeout()/requestAnimationFrame() callbacks.
 *
 * The bridge itself is clock-agnostic: the embedder owns a monotonic
 * millisecond clock and passes its current reading as `now_ms`. The bridge
 * accumulates the delta between successive calls and fires every timer whose
 * countdown has elapsed, invoking each callback synchronously on the VM (the
 * embedder must hold whatever lock protects the DOM, exactly as during a
 * vm_run()). The first call only seeds the clock and fires nothing.
 *
 * Returns the number of callbacks fired; >0 means the page likely mutated and
 * should be repainted. Returns 0 when no timers are registered. Safe to call
 * from a UI tick at any cadence. */
int js_dom_poll_timers(vm_t* vm, uint64_t now_ms);

/* Drop every cached handle -> Element-wrapper mapping.
 *
 * Element handles are raw pointers into the embedder's document tree, and the
 * bridge caches the JS wrapper built for each one so that repeated lookups of
 * the same node return the same object. Call this whenever that tree is
 * destroyed or rebuilt while the VM stays alive - a document.write() reparse
 * is the usual case - because a wrapper left in the cache would hand the next
 * script a pointer into freed memory.
 *
 * Wrapper objects a script still references are NOT freed (the cache only
 * holds one reference each); they simply stop being reachable by handle, and
 * the next wrap of a live element builds a fresh wrapper. Element identity
 * across the reset is therefore lost, which is the correct trade: the nodes
 * the old wrappers pointed at no longer exist.
 *
 * Pair this with js_event_clear_listeners(), which drops the listeners keyed
 * by the same handles. Safe to call when the bridge is not installed. */
void js_dom_reset_element_cache(vm_t* vm);

/* ------------------------------------------------------------------ */
/* Accessors for sibling bridges (js_event.c, js_web.c, js_canvas.c)   */
/*                                                                    */
/* The DOM bridge owns the embedder context and the callback table, so */
/* the other browser bridges reach them through the vm instead of      */
/* making the embedder plumb the same pointers twice.                  */
/* ------------------------------------------------------------------ */

/* Opaque embedder context registered with js_register_dom_natives(). */
void* js_dom_ctx(vm_t* vm);

/* Copy of the callback table registered with js_register_dom_natives().
 * Returns NULL when the DOM bridge is not installed on `vm`. The pointer is
 * valid for the lifetime of the vm. */
const js_dom_callbacks_t* js_dom_callbacks(vm_t* vm);

/* Wrap an embedder element handle in a JS Element instance (or JS null when
 * `el` is NULL). Used by the event bridge to expose Event.target. */
var_t* js_dom_wrap_element(vm_t* vm, js_element_t el);

/* Wrap an element handle in a CSSStyleDeclaration, the same object type
 * `element.style` returns. js_web.c uses it for getComputedStyle(), which
 * then reads through the embedder's computed-style callback for free.
 * Returns JS null when `el` is NULL. */
var_t* js_dom_wrap_style(vm_t* vm, js_element_t el);

/* The Element class var registered on `vm` (NULL when the DOM bridge is not
 * installed). Lets a sibling bridge tell Element wrappers from plain objects
 * without repeating the class-name lookup. */
var_t* js_dom_element_class(vm_t* vm);

/* Timer table entry points, used by requestAnimationFrame() and by embedders
 * that want to schedule a JS callback themselves. js_dom_add_timer returns a
 * positive timer id, or 0 when the callback is not a function / the table is
 * full. `cb` is anchored against the GC by the bridge. */
int  js_dom_add_timer(vm_t* vm, var_t* cb, uint32_t ms, bool repeat);
void js_dom_clear_timer(vm_t* vm, int id);

/* Raw CLOCK_MONOTONIC milliseconds (not relative to VM creation - it is a
 * plain monotonic reading, so only differences are meaningful). Exposed so
 * performance.now() and the web bridge share one clock source. */
int64_t js_dom_monotonic_ms(void);

/* Wall-clock milliseconds since the Unix epoch, the same reading Date.now()
 * and `new Date()` use. performance.timeOrigin is derived from this so that
 * timeOrigin + now() lines up with Date.now(). */
int64_t js_dom_wall_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* MARIO_JS_DOM_H */
