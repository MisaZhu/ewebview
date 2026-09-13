/*
 * js_event.h - DOM Event / EventTarget natives for the mario JavaScript VM.
 *
 * Companion to js_dom.h and built on top of it: this bridge reuses the DOM
 * bridge's embedder context, callback table, Element wrappers and timer
 * table (see js_dom_ctx()/js_dom_callbacks()/js_dom_wrap_element()), so the
 * embedder registers its callbacks exactly once, with js_register_dom_natives().
 *
 * It is still pure C: it knows nothing about WidgetWebview, litehtml or
 * EwokOS. Native input events reach JavaScript through js_event_dispatch().
 *
 * Registration order (each step needs the previous one):
 *   1. vm_init()
 *   2. js_register_dom_natives(vm, ctx, &cb)   - installs Document/Element
 *   3. js_register_canvas_natives(...)         - optional
 *   4. js_register_event_natives(vm)           - installs Event/EventTarget
 *   5. js_register_web_natives(vm, &web_cb)    - optional
 *
 * Supported surface:
 *   new Event(type[, {bubbles, cancelable}])
 *   new CustomEvent(type[, {bubbles, cancelable, detail}])
 *   new MouseEvent(type[, init]) / new KeyboardEvent(type[, init])
 *   ev.type/bubbles/cancelable/defaultPrevented/eventPhase/isTrusted
 *   ev.target/currentTarget/srcElement/timeStamp/cancelBubble
 *   ev.preventDefault()/stopPropagation()/stopImmediatePropagation()
 *   ev.initEvent()/composedPath()
 *   MouseEvent: clientX/Y, pageX/Y, screenX/Y, offsetX/Y, x/y, button,
 *               buttons, detail, relatedTarget, altKey/ctrlKey/shiftKey/metaKey
 *   KeyboardEvent: key, code, keyCode, charCode, which, location, repeat,
 *               altKey/ctrlKey/shiftKey/metaKey
 *   EventTarget (Element, Document, window):
 *      addEventListener(type, fn[, capture|{capture,once}])
 *      removeEventListener(type, fn[, capture])
 *      dispatchEvent(event)
 *      onclick / onload / oninput / ... (the full kEventTypes list) as
 *      assignable properties, on Element, Document and window alike
 *   Full capture -> target -> bubble propagation with stopPropagation.
 *   Inline HTML handlers: <div onclick="..."> is compiled on demand and
 *      invoked with `this` = the element and `event` bound.
 *
 * Threading: every entry point runs on the VM thread. js_event_dispatch()
 * may be called from the embedder between vm_run() calls; the embedder must
 * hold whatever lock protects the DOM, exactly as for js_dom_poll_timers().
 */

#ifndef MARIO_JS_EVENT_H
#define MARIO_JS_EVENT_H

#include "mario.h"
#include "js_dom.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Modifier keys, OR-ed into js_event_init_t::mods. */
#define JS_EVENT_MOD_ALT    1
#define JS_EVENT_MOD_CTRL   2
#define JS_EVENT_MOD_SHIFT  4
#define JS_EVENT_MOD_META   8

/* Which object an event is aimed at. ELEMENT uses init->target as an opaque
 * handle handed out by the DOM bridge; DOCUMENT/WINDOW ignore it. */
#define JS_EVENT_ON_ELEMENT   0
#define JS_EVENT_ON_DOCUMENT  1
#define JS_EVENT_ON_WINDOW    2

typedef struct js_event_init {
    const char*  type;        /* required, e.g. "click"; case-insensitive */
    int          on;          /* JS_EVENT_ON_* */
    js_element_t target;      /* element handle; NULL for document/window */

    bool         bubbles;
    bool         cancelable;
    bool         trusted;     /* false for script-created events */

    /* MouseEvent fields (ignored by non-mouse events). */
    int          client_x, client_y;
    int          screen_x, screen_y;
    /* Document scroll offsets, added to client_x/y to produce pageX/pageY.
     * Leave 0 when the embedder has no scrollable viewport; pageX then equals
     * clientX, which is correct for an unscrolled document. */
    int          scroll_x, scroll_y;
    int          button;      /* 0 left, 1 middle, 2 right */
    int          buttons;     /* bitmask of currently pressed buttons */
    int          detail;      /* click count; CustomEvent ignores this */

    /* KeyboardEvent fields (ignored by non-key events). */
    const char*  key;         /* "a", "Enter", "ArrowLeft" ... */
    const char*  code;        /* "KeyA", "Enter" ... */
    int          key_code;    /* legacy keyCode / which / charCode */
    bool         repeat;

    unsigned     mods;        /* JS_EVENT_MOD_* */

    /* CustomEvent payload. Borrowed: the bridge refs it for the duration of
     * the dispatch only, so the caller keeps ownership. */
    var_t*       custom_detail;
} js_event_init_t;

/* Zero an init struct and fill in the mandatory `type`. The remaining fields
 * default to what the DOM specifies (bubbles/cancelable false, coords 0). */
void js_event_init(js_event_init_t* init, const char* type);

/* Install Event/CustomEvent/MouseEvent/KeyboardEvent plus the EventTarget
 * methods and the on* handler properties on the Document/Element classes and
 * on `window`.
 *
 * MUST be called after js_register_dom_natives() (it looks up the Document
 * and Element classes and the `window`/`document` globals it created) and
 * before any script runs. Returns false when the DOM bridge is missing. */
bool js_register_event_natives(vm_t* vm);

/* ------------------------------------------------------------------ */
/* Native event delivery                                              */
/* ------------------------------------------------------------------ */

/* Run the full capture/target/bubble propagation for one event and return
 * false when a listener called preventDefault() on a cancelable event (the
 * embedder should then skip its default action, e.g. following a link).
 * Returns true when nothing was cancelled, including when no listener ran.
 *
 * Safe to call with a NULL/absent bridge: it then returns true and does
 * nothing, so input paths need no guards. */
bool js_event_dispatch(vm_t* vm, const js_event_init_t* init);

/* Shorthands for the events an embedder dispatches most often.
 *
 * js_event_dispatch_mouse takes the pointer position in CLIENT coordinates
 * (relative to the visible viewport) plus the document's current scroll
 * offsets, from which it derives pageX/pageY. */
bool js_event_dispatch_simple(vm_t* vm, js_element_t el, const char* type, bool bubbles);
bool js_event_dispatch_mouse(vm_t* vm, js_element_t el, const char* type,
                             int x, int y, int scroll_x, int scroll_y,
                             int button, int click_count);
bool js_event_dispatch_key(vm_t* vm, js_element_t el, const char* type,
                           const char* key, int key_code, unsigned mods);
bool js_event_dispatch_custom(vm_t* vm, js_element_t el, const char* type,
                              var_t* detail, bool bubbles);

/* Window/document lifecycle events, all dispatched on the matching global.
 * js_event_fire_load() runs document "load", window "load" and then the
 * <body onload> attribute handler (HTML treats that attribute as the window's
 * load slot). */
void js_event_fire_dom_content_loaded(vm_t* vm);
void js_event_fire_load(vm_t* vm);
void js_event_fire_resize(vm_t* vm, int width, int height);
void js_event_fire_scroll(vm_t* vm);
void js_event_fire_unload(vm_t* vm);
void js_event_fire_error(vm_t* vm, const char* message);

/* Fire the `<body onload>` attribute handler and any listener registered on
 * the body element. Called by js_event_fire_load(); exposed separately so an
 * embedder that builds a page incrementally can trigger it at its own moment. */
void js_event_fire_body_load(vm_t* vm);

/* Number of listeners currently registered (diagnostics / tests). */
int js_event_listener_count(vm_t* vm);

/* Drop every registered listener. Call this before the embedder destroys or
 * re-parses the document: element handles are only valid for the lifetime of
 * a page, and a listener left behind would be invoked with a stale one. */
void js_event_clear_listeners(vm_t* vm);

#ifdef __cplusplus
}
#endif

#endif /* MARIO_JS_EVENT_H */
