/*
 * ewebview.h - a self-contained HTML/CSS/JavaScript web engine.
 *
 * ewebview renders a web page into an abstract ARGB8888 memory canvas and
 * drives network / font / image / clock work through the platform porting
 * tables in <ewebview_port.h>. It has NO dependency on EwokOS, xwin, widget++,
 * graph_t or any window system: those live entirely in a port. An embedder
 *
 *   1. fills an eweb_port_t for its platform (or calls eweb_port_ewokos()),
 *   2. creates an ewebview_t and registers an eweb_listener_t,
 *   3. sets the viewport and calls ewebview_load(url),
 *   4. on its UI thread, forwards input with ewebview_post_event() /
 *      ewebview_scroll() and pumps ewebview_tick(), adopting the frames the
 *      listener hands back and blitting them into its own window.
 *
 * ---------------------------------------------------------------------------
 * Threading model (mirrors the original xBrowser WidgetWebview)
 * ---------------------------------------------------------------------------
 * ewebview owns TWO kinds of thread, created with pthread (assumed available
 * on every target; only drawing/font/image/net/clock are abstracted):
 *
 *   - the ENGINE thread: owns the litehtml documents, the mario VM, all
 *     parsing/styling/layout/rasterization and the offscreen frame pool. It
 *     never touches the embedder's window.
 *   - on-demand DOWNLOAD worker threads: fetch + decode subresources off the
 *     engine thread and push results the engine drains.
 *
 * The embedder's UI thread talks to the engine ONLY through ewebview_load /
 * _stop / _reload / _set_viewport / _scroll / _post_event / _set_js_enabled,
 * every one of which just queues a command and returns immediately (the window
 * never blocks on a fetch/parse), and through ewebview_tick(), which drains the
 * engine->UI event queue and fires the listener callbacks. All listener
 * callbacks therefore run on the UI thread, inside ewebview_tick().
 *
 * Frames: the engine renders the viewport into a pooled surface and hands
 * ownership to the embedder via on_frame(); the embedder adopts it (typically
 * as its display cache, blitting it in its own repaint) and returns it with
 * ewebview_release_frame(), usually when it adopts the next one. Until released
 * the engine will not draw into that surface - natural back-pressure with no
 * per-frame allocation and no pixel copy across the thread boundary.
 */

#ifndef EWEBVIEW_H
#define EWEBVIEW_H

#include <ewebview_port.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque engine instance. */
typedef struct ewebview ewebview_t;

/* ------------------------------------------------------------------ */
/* Input events (the embedder -> engine direction)                     */
/* ------------------------------------------------------------------ */

/* Mouse gesture state, mapped 1:1 from the platform's pointer events by the
 * embedder. The engine dispatches the matching DOM mouse events and follows
 * <a href> clicks; it ignores EWEB_MOUSE_WHEEL (the embedder turns a wheel
 * gesture into ewebview_scroll()). */
enum {
    EWEB_MOUSE_MOVE = 0,
    EWEB_MOUSE_DOWN,
    EWEB_MOUSE_UP,
    EWEB_MOUSE_CLICK,
    EWEB_MOUSE_DOUBLE_CLICK,
    EWEB_MOUSE_WHEEL,
};

/* Mouse button. */
enum {
    EWEB_BUTTON_NONE = 0,
    EWEB_BUTTON_LEFT,
    EWEB_BUTTON_MIDDLE,
    EWEB_BUTTON_RIGHT,
};

typedef struct eweb_event {
    int mouse_state;   /* one of EWEB_MOUSE_* */
    int button;        /* one of EWEB_BUTTON_* */
    int cx;            /* client X, relative to the viewport top-left */
    int cy;            /* client Y, relative to the viewport top-left */
    int wheel;         /* EWEB_MOUSE_WHEEL only: -1 up, +1 down (lines) */
} eweb_event_t;

/* Keyboard gesture state, mapped from the platform's key events by the
 * embedder. Text entry and key semantics travel separately: a printable
 * character arrives as one EWEB_KEYSTATE_DOWN with key==EWEB_KEY_CHAR and the
 * UTF-8 bytes in `text` (this is the IME-correct path), while every physical
 * key - printable or not - also arrives as a plain EWEB_KEYSTATE_DOWN/UP
 * carrying only `key`+`mods`, which is what the DOM keydown/keyup events and
 * the navigation/editing shortcuts are driven from. */
enum {
    EWEB_KEYSTATE_DOWN = 0,
    EWEB_KEYSTATE_UP,
};

/* Virtual key codes. EWEB_KEY_CHAR means "a printable character follows in
 * text[]"; the rest name the non-printable keys the engine acts on. Letters
 * and digits are their ASCII code points so the embedder can forward them
 * verbatim. */
enum {
    EWEB_KEY_CHAR = 0,      /* printable: the UTF-8 char is in text[] */
    EWEB_KEY_BACKSPACE = 8,
    EWEB_KEY_TAB = 9,
    EWEB_KEY_ENTER = 13,
    EWEB_KEY_ESCAPE = 27,
    EWEB_KEY_SPACE = 32,
    EWEB_KEY_DELETE = 127,
    EWEB_KEY_LEFT = 0x1000,
    EWEB_KEY_RIGHT,
    EWEB_KEY_UP,
    EWEB_KEY_DOWN,
    EWEB_KEY_HOME,
    EWEB_KEY_END,
    EWEB_KEY_PAGEUP,
    EWEB_KEY_PAGEDOWN,
    EWEB_KEY_INSERT,
    EWEB_KEY_F1, EWEB_KEY_F2, EWEB_KEY_F3, EWEB_KEY_F4,
    EWEB_KEY_F5, EWEB_KEY_F6, EWEB_KEY_F7, EWEB_KEY_F8,
    EWEB_KEY_F9, EWEB_KEY_F10, EWEB_KEY_F11, EWEB_KEY_F12,
};

/* Modifier bitmask carried in eweb_key_event_t::mods. */
enum {
    EWEB_MOD_SHIFT = 1 << 0,
    EWEB_MOD_CTRL  = 1 << 1,
    EWEB_MOD_ALT   = 1 << 2,
    EWEB_MOD_META  = 1 << 3,   /* Cmd on macOS, Win/Super elsewhere */
};

typedef struct eweb_key_event {
    int  type;      /* EWEB_KEYSTATE_DOWN / EWEB_KEYSTATE_UP */
    int  key;       /* one of EWEB_KEY_* */
    int  mods;      /* EWEB_MOD_* bitwise-or */
    char text[8];   /* UTF-8 character when key==EWEB_KEY_CHAR, else empty */
} eweb_key_event_t;

/* Sub-resource task kinds reported through the on_task_* listener hooks. */
enum {
    EWEB_TASK_HTML = 0,
    EWEB_TASK_CSS,
    EWEB_TASK_IMAGE,
    EWEB_TASK_SCRIPT,
};

/* ------------------------------------------------------------------ */
/* Listener (the engine -> embedder direction, all on the UI thread)    */
/* ------------------------------------------------------------------ */

typedef struct eweb_listener {
    void* ud;

    /* A rendered viewport frame is ready. OWNERSHIP of `frame` transfers to the
     * embedder: adopt it (blit it into the window now, or cache it and blit in
     * the next repaint) and eventually hand it back with
     * ewebview_release_frame(). frame_scroll_x/y is the document offset the
     * frame was rendered at (the live scroll may lead it during a fast scroll,
     * so blit shifted by frameScroll - liveScroll). doc_w/doc_h is the full
     * document size, for sizing a scrollbar. */
    void (*on_frame)(void* ud, eweb_surface_t* frame,
                     int frame_scroll_x, int frame_scroll_y, int doc_w, int doc_h);

    /* Engine-authoritative scroll offset (page swap -> 0, window.scrollTo, or a
     * script run end). Snap the embedder's live offset + scrollbar to it. */
    void (*on_scroll)(void* ud, int x, int y, int doc_w, int doc_h);

    /* The visible page's URL changed (address-bar load or an in-page
     * navigation). The embedder owns session history, so record it here. */
    void (*on_url)(void* ud, const char* url);

    /* Document title (from <title> or document.title). */
    void (*on_title)(void* ud, const char* title);

    /* Status-bar text (link target, load progress messages, alert() text). */
    void (*on_status)(void* ud, const char* text, int progress);

    /* Build-overlay state: `overlay` is true only while a real build (not a
     * post-swap script run) is in flight, so the embedder can veil the page and
     * show `text` + a `progress` (0..100) bar. */
    void (*on_build_status)(void* ud, const char* text, int progress, bool overlay);

    /* Requested cursor shape (e.g. "pointer"). May be NULL/"default". */
    void (*on_cursor)(void* ud, const char* cursor);

    /* alert()/confirm()/prompt() text, surfaced non-blocking (the engine never
     * waits on a modal; confirm answers "cancel", prompt answers null). */
    void (*on_dialog)(void* ud, const char* text);

    /* Sub-resource task lifecycle, for a progress/throbber UI. */
    void (*on_task_start)(void* ud, const char* url, int type);
    void (*on_task_end)(void* ud, const char* url, int type);
    void (*on_task_failed)(void* ud, const char* url, int type);
    void (*on_tasks_end)(void* ud);
} eweb_listener_t;

/* Zero an eweb_listener_t. Handy for embedders that only implement a few hooks. */
void eweb_listener_init(eweb_listener_t* l);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Create an engine driven by `port` (copied by value). Returns NULL on
 * allocation failure or when a REQUIRED table (gfx/font/clock) is missing.
 * The engine thread starts immediately and parks until a load is queued. */
ewebview_t* ewebview_create(const eweb_port_t* port);

/* Stop the engine + download workers, tear down the documents/VM/frame pool
 * and free everything. Any frame the embedder still holds becomes invalid, so
 * release adopted frames first. Safe to call with NULL. */
void ewebview_destroy(ewebview_t* v);

/* Install the UI callback table (copied by value). May be called again to
 * change or clear (NULL) the listener. UI-thread. */
void ewebview_set_listener(ewebview_t* v, const eweb_listener_t* listener);

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/* Resize the laid-out viewport (CSS pixels). Queues a re-layout + re-render and
 * fires window.onresize. UI-thread; returns immediately. */
void ewebview_set_viewport(ewebview_t* v, int width, int height);

/* Set the master stylesheet URL (e.g. "file:/.../master.css"), loaded before
 * the first page. UI-thread. */
void ewebview_set_default_css(ewebview_t* v, const char* url);

/* Enable/disable JavaScript. Enabled by default; disabling frees the VM and
 * makes the next page load drop <script> bodies. UI-thread. */
void ewebview_set_js_enabled(ewebview_t* v, bool enabled);

/* ------------------------------------------------------------------ */
/* Navigation (UI-thread; each queues a command and returns at once)    */
/* ------------------------------------------------------------------ */

/* Begin loading `url`. Aborts + cleans up any in-flight page first. */
void ewebview_load(ewebview_t* v, const char* url);

/* Stop the in-flight load, keeping whatever is already on screen. */
void ewebview_stop(ewebview_t* v);

/* Reload the current page. */
void ewebview_reload(ewebview_t* v);

/* URL of the page currently on screen (empty before the first load). The
 * returned pointer is owned by the engine and stays valid until the next
 * navigation or ewebview_destroy(). UI-thread. */
const char* ewebview_get_url(ewebview_t* v);

/* ------------------------------------------------------------------ */
/* Input (UI-thread)                                                   */
/* ------------------------------------------------------------------ */

/* Forward one pointer gesture. cx/cy are viewport-relative client coords the
 * embedder computed (the engine cannot walk the embedder's window geometry).
 * Queued to the engine, which dispatches the DOM mouse events and follows
 * <a href> clicks there. */
void ewebview_post_event(ewebview_t* v, const eweb_event_t* ev);

/* Forward one keyboard gesture to the focused page element (see
 * eweb_key_event_t). Queued to the engine, which dispatches the DOM
 * keydown/keypress/keyup events there and, unless a handler cancels them,
 * runs the matching default action (text editing, control activation, focus
 * traversal). Send printable characters via SDL_TEXTINPUT-style
 * key==EWEB_KEY_CHAR events and every physical key via its own DOWN/UP. */
void ewebview_post_key(ewebview_t* v, const eweb_key_event_t* ev);

/* Scroll to an absolute document offset (the embedder clamps it to the last
 * known doc geometry first, moves its live offset for an immediate blit-shift,
 * then calls this so the engine re-renders the exposed strip and fires the
 * page's scroll handlers). */
void ewebview_scroll(ewebview_t* v, int x, int y);

/* True when the engine has no build, style walk, layout or sub-resource work
 * outstanding: the most recently delivered frame is then the final visual
 * state for the current resource set. Heuristic for capture/automation hooks
 * (a screenshot taken while this is false can catch a mid-cascade frame);
 * animations do not count as work. UI-thread, cheap, never blocks. */
bool ewebview_is_idle(ewebview_t* v);

/* ------------------------------------------------------------------ */
/* UI pump + frame ownership (UI-thread)                               */
/* ------------------------------------------------------------------ */

/* Drain the engine->UI event queue, firing the listener callbacks (on_frame,
 * on_scroll, on_url, on_status, on_build_status, on_task_*, ...). Call this
 * once per UI tick (e.g. from the embedder's timer). */
void ewebview_tick(ewebview_t* v);

/* Return an adopted frame to the engine's pool (or free it if a resize changed
 * its size). Call once the embedder is done blitting a frame it adopted in
 * on_frame() - typically right before adopting the next one. Must be a surface
 * the engine handed over via on_frame() and not yet released. */
void ewebview_release_frame(ewebview_t* v, eweb_surface_t* frame);

#ifdef __cplusplus
}
#endif

#endif /* EWEBVIEW_H */
