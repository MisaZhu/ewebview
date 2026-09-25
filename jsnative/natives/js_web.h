/*
 * js_web.h - Browser "BOM" natives for the mario JavaScript VM.
 *
 * Third bridge in the set (js_dom.h = Document/Element, js_event.h =
 * Event/EventTarget, this one = everything else a page expects to find on
 * `window`). Like the others it is pure C and knows nothing about
 * WidgetWebview, litehtml or EwokOS.
 *
 * The embedder context is shared: this bridge reaches it through
 * js_dom_ctx(vm), so js_web_callbacks_t only carries the hooks the DOM bridge
 * does not already have. Every callback is OPTIONAL; a NULL one makes the
 * corresponding API degrade (empty string / 0 / false / no-op) instead of
 * crashing, so an embedder can adopt this incrementally.
 *
 * Registration order:
 *   vm_init() -> js_register_dom_natives() -> js_register_canvas_natives()
 *             -> js_register_event_natives() -> js_register_web_natives()
 *
 * Supported surface:
 *   Global functions: parseInt parseFloat isNaN isFinite
 *      encodeURIComponent decodeURIComponent encodeURI decodeURI
 *      escape unescape atob btoa queueMicrotask
 *   console: warn error info debug trace dir assert count group groupEnd
 *      time timeLog timeEnd clear (log/write come from the engine)
 *   localStorage / sessionStorage: getItem setItem removeItem clear key length
 *   location: href protocol host hostname port pathname search hash origin
 *      assign() replace() reload() toString()
 *   history: length state back() forward() go() pushState() replaceState()
 *   navigator: userAgent appVersion appName appCodeName product vendor
 *      platform language languages onLine cookieEnabled hardwareConcurrency
 *      maxTouchPoints webdriver doNotTrack javaEnabled() sendBeacon()
 *   screen: width height availWidth availHeight colorDepth pixelDepth
 *      orientation
 *   performance: now() timeOrigin mark() measure() clearMarks()
 *      clearMeasures() navigation memory
 *   window: innerWidth/innerHeight outerWidth/outerHeight screenX/screenY
 *      scrollX/scrollY pageXOffset/pageYOffset devicePixelRatio origin name
 *      status closed length self top parent frames globalThis
 *      scrollTo() scrollBy() scroll() moveTo() moveBy() resizeTo() resizeBy()
 *      close() focus() blur() print() open() stop() confirm() prompt()
 *      getComputedStyle() matchMedia()
 *   document: cookie hidden visibilityState hasFocus() activeElement
 *      compatMode contentType lastModified forms links images scripts
 *   XMLHttpRequest (synchronous, driven by the http_request callback) and
 *      fetch(), which returns a Promise<Response> built on the same callback.
 *      Response: ok status statusText url type headers text() json().
 *      Headers: get() has().
 *   crypto: getRandomValues() randomUUID()
 *
 * Known gaps (deliberate, not oversights):
 *   - window.postMessage() accepts the call and does nothing: there are no
 *     frames, workers or popups to talk to.
 *   - globalThis aliases `window`, which is NOT the same object as mario's
 *     global scope (vm->root). Reading globals through it works; assigning a
 *     new global through it does not become visible as a bare identifier.
 *   - performance.mark()/measure() are accepted but keep no timeline.
 *   - crypto.getRandomValues() is seeded from the monotonic clock, so it is
 *     suitable for ids and client-side keys only, never for real secrets.
 *   - XMLHttpRequest is always synchronous, so `async` in open() is ignored.
 */

#ifndef MARIO_JS_WEB_H
#define MARIO_JS_WEB_H

#include "mario.h"
#include "js_dom.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct js_web_callbacks {
    /* ---- modal dialogs ----
     * confirm: NULL => always true. prompt: NULL => always JS null (cancel);
     * otherwise returns a mario_malloc'd string, or NULL when the user
     * cancelled. */
    bool  (*confirm)(void* ctx, const char* message);
    char* (*prompt)(void* ctx, const char* message, const char* def);

    /* ---- viewport / screen metrics, in CSS pixels ----
     * Any out pointer may be NULL. A NULL callback falls back to 0, which
     * pages treat as "unknown". */
    void  (*get_viewport)(void* ctx, int* w, int* h);
    void  (*get_screen)(void* ctx, int* w, int* h, int* depth);
    void  (*get_scroll)(void* ctx, int* x, int* y);
    void  (*scroll_to)(void* ctx, int x, int y);
    /* Preferred color scheme in the litehtml media_features encoding
     * (0 = dark, 1 = light), for matchMedia("(prefers-color-scheme: ...)").
     * NULL reports light, the same default the CSS side applies when no
     * scheme is configured; JS and CSS must agree or a page that gates its
     * theme on matchMedia paints the opposite scheme from its stylesheets. */
    int   (*get_color_scheme)(void* ctx);

    /* ---- navigation ----
     * navigate backs location.assign()/location.href = .../window.open().
     * NULL makes navigation a no-op while location.href still reports the
     * current document URL (the DOM bridge's get_url). */
    void  (*navigate)(void* ctx, const char* url);
    void  (*reload)(void* ctx);
    /* Soft URL replacement for history.pushState/replaceState: adopts the new
     * URL reported by location.* and the address bar WITHOUT refetching or
     * tearing down the document. NULL => pushState/replaceState only remember
     * the state object and leave the reported URL unchanged. */
    void  (*update_url)(void* ctx, const char* url);
    void  (*history_back)(void* ctx);
    void  (*history_forward)(void* ctx);
    int   (*history_length)(void* ctx);

    /* ---- identity ----
     * All three return a mario_malloc'd string or NULL. NULL falls back to a
     * built-in default so navigator.userAgent is never empty (feature
     * detection scripts read it unconditionally). */
    char* (*get_user_agent)(void* ctx);
    char* (*get_language)(void* ctx);
    char* (*get_platform)(void* ctx);

    /* ---- document.cookie ----
     * get_cookie returns the full "k=v; k2=v2" header, mario_malloc'd or
     * NULL. set_cookie receives one Set-Cookie-shaped string; the embedder is
     * responsible for stripping the path/expires attributes. */
    char* (*get_cookie)(void* ctx);
    void  (*set_cookie)(void* ctx, const char* cookie);

    /* ---- Web Storage persistence ----
     * `session` false => localStorage, true => sessionStorage. The blob uses
     * the length-prefixed format js_web_storage_dump() produces; load returns
     * a mario_malloc'd blob or NULL when there is nothing stored. Both NULL
     * => the store lives only as long as the VM.
     *
     * save is called after every mutation, on the VM thread. */
    char* (*storage_load)(void* ctx, bool session);
    void  (*storage_save)(void* ctx, bool session, const char* blob);

    /* ---- synchronous HTTP (XMLHttpRequest / fetch) ----
     * Runs on the VM thread, so it MUST block until the response is complete.
     * `headers` is a "\r\n"-joined request header block (may be NULL/empty),
     * `body` may be NULL. On success fills *status with the HTTP status and
     * returns true with *out_body set to a mario_malloc'd NUL-terminated
     * response body and *out_headers to a mario_malloc'd "\r\n"-joined
     * response header block (either may be NULL). Return false for a network
     * error, which makes XHR report readyState 4 / status 0 exactly like a
     * browser does for a failed request.
     *
     * NULL => XMLHttpRequest is present but every request fails, so pages
     * that probe for it do not throw. */
    bool  (*http_request)(void* ctx, const char* method, const char* url,
                          const char* headers, const char* body,
                          int* status, char** out_body, char** out_headers);
} js_web_callbacks_t;

/* Zero a callback struct. Handy for embedders that only fill a few hooks. */
void js_web_callbacks_init(js_web_callbacks_t* cb);

/* Install the window/navigator/screen/location/history/performance/storage/
 * console surface on `vm`. `cb` may be NULL, in which case every hook is
 * treated as absent and the API still installs (reporting defaults).
 *
 * Requires js_register_dom_natives() to have run: this bridge reuses its
 * embedder context, its Location class and its window/document globals.
 * Returns false when vm is NULL or the DOM bridge is missing. */
bool js_register_web_natives(vm_t* vm, const js_web_callbacks_t* cb);

/* ------------------------------------------------------------------ */
/* Storage helpers                                                    */
/* ------------------------------------------------------------------ */

/* Serialize one store into the persistence format: a sequence of
 * "<klen>:<key><vlen>:<value>" records, which is binary-safe (values may
 * contain any byte, including ':' and newlines). Returns a mario_malloc'd
 * NUL-terminated string, never NULL. */
char* js_web_storage_dump(vm_t* vm, bool session);

/* Replace one store's contents from a blob produced by js_web_storage_dump().
 * A NULL/empty blob clears the store. Malformed records are skipped. */
void js_web_storage_restore(vm_t* vm, bool session, const char* blob);

/* The Storage instance for `session` (JS null when the bridge is missing). */
var_t* js_web_storage(vm_t* vm, bool session);

#ifdef __cplusplus
}
#endif

#endif /* MARIO_JS_WEB_H */
