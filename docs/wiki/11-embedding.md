# Chapter 11 · Embedding Guide

> Language: **English** | [中文](11-embedding.zh.md)

This chapter is for readers embedding ewebview into their own application: the calling order of the C API, the ownership contracts of frames and scrolling, the semantics of every listener hook, and finally an anatomy of the reference examples `WidgetWebview` + `xBrowser`. All APIs are in `ewebview/include/ewebview.h`.

## 11.1 Minimal Embedding Sequence

```c
#include <ewebview.h>
#include <ewebview_port.h>

/* 1. Porting table: use a reference port, or fill your own per Ch. 10 */
eweb_port_t port;
eweb_port_init(&port);
eweb_port_ewokos(&port, NULL);          /* the EwokOS reference port */
/* or desktop (the default build's port; declare it extern yourself):
   extern void eweb_port_sdl2(eweb_port_t*, void*);
   eweb_port_sdl2(&port, NULL);          // SDL_Init(VIDEO) first, optionally _set_dpr() */

/* 2. Create the engine (the engine thread starts and parks immediately); returns NULL if gfx/font/clock are missing */
ewebview_t* v = ewebview_create(&port);

/* 3. Install listeners (fill only the hooks you care about, leave the rest NULL) */
eweb_listener_t lis;
eweb_listener_init(&lis);
lis.ud       = my_ctx;
lis.on_frame = my_on_frame;             /* frame delivery, the only truly mandatory hook */
ewebview_set_listener(v, &lis);

/* 4. Viewport + navigation */
ewebview_set_viewport(v, win_w, win_h);
ewebview_load(v, "https://example.com/");

/* 5. UI main loop: pump once per tick (e.g. a 30Hz timer) */
ewebview_tick(v);                       /* all listener callbacks fire here */

/* 6. Input and scrolling (both queue-and-return) */
ewebview_post_event(v, &ev);
ewebview_scroll(v, x, y);

/* 7. Destruction: return all adopted frames first, then destroy */
ewebview_release_frame(v, held_frame);
ewebview_destroy(v);
```

## 11.2 Threading Contract (Emphasized Again)

- Every **UI → engine** API (`load/stop/reload/set_viewport/scroll/post_event/set_js_enabled`) merely **pushes a command into the command queue and returns immediately** — the window is never stuck by a fetch/parse;
- All **engine → UI** callbacks fire **on the UI thread, inside `ewebview_tick()`**. So a callback must **never block** (blocking tick means blocking the whole browser UI), and must not re-enter engine state other than the engine APIs from within a callback;
- The engine and download threads are private to the engine (Ch. 3); the embedder never needs to touch them.

## 11.3 Frame Delivery & Ownership

```c
void my_on_frame(void* ud, eweb_surface_t* frame,
                 int frame_scroll_x, int frame_scroll_y, int doc_w, int doc_h);
```

- **Ownership transfers with the callback**: the embedder adopts the frame (usually as a display cache, blitting it in its own repaint) and returns it via `ewebview_release_frame()` after use — generally returning the previous frame before adopting the next one;
- **Ensure you no longer read its pixels before returning it**: once `ewebview_release_frame()` is called, the surface returns to the engine frame pool immediately and the next render may overwrite it right away. If you still need to copy its pixels (e.g. sdlbrowser uploading the `SDL_Surface` into a streaming `SDL_Texture`), you must **finish copying before returning it**: sdlbrowser parks the old frame in `frame_prev` and releases it in `upload_frame()` only after the new frame has been uploaded into the texture (otherwise you get tearing rows / ghost text / half-drawn images);
- **Backpressure**: while a frame is unreturned the engine does not paint onto that surface again — naturally limited to a frame count, with no cross-thread pixel copy and no per-frame allocation;
- **Zero copy**: when the embedder and the port are on the same platform, the frame handle is the platform's native surface. On EwokOS `frameGraph()` casts the `eweb_surface_t*` straight back to `graph_t*` and blits it into the window (equivalent to a shortcut for `surface_native()`); on SDL2 the `eweb_surface_t*` wraps an `SDL_Surface*`, and sdlbrowser uploads it into a streaming `SDL_Texture` then `SDL_RenderCopy`s it into the content area;
- `doc_w/doc_h` are the full document size, used to scale the scrollbar.

## 11.4 The Scrolling Model: Displacement blit + Authoritative Receipt

Scrolling is a **duet** between the embedder and the engine, aimed at zero-latency drag/wheel scrolling:

```
User scroll gesture
  │  1. The embedder clamps the target offset by the last-known document geometry
  │  2. Immediately moves its own live offset → onRepaint blits the cached frame
  │     displaced by (frameScroll - liveScroll) (the page follows the finger instantly)
  │  3. ewebview_scroll(x, y) notifies the engine
  ▼
Engine thread: update the scroll offset → repaint the exposed strip → trigger the page's scroll handler
  │  4. on_frame brings a full frame rendered at the new offset
  │  5. The authoritative event on_scroll(x, y, doc_w, doc_h): fired on page-change zeroing,
  │     window.scrollTo, and end of a script run → the embedder snaps
  │     its live offset and scrollbar to it
```

The displacement formula in `WidgetWebview::onRepaint` (`WidgetWebview.cc`):

```cpp
int dx = r.x + (m_frameScrollX - m_scrollX);
int dy = r.y + (m_frameScrollY - m_scrollY);
```

sdlbrowser's version of the same formula adds HiDPI scaling (scroll offsets are logical CSS px, the blit runs in device px, `bin/sdlbrowser/main.c`):

```c
int dx = cr.x + lroundf((b->frame_sx - b->scroll_x) * b->ui_scale);
int dy = cr.y + lroundf((b->frame_sy - b->scroll_y) * b->ui_scale);
```

Note that the **engine ignores `EWEB_MOUSE_WHEEL` directly** — the wheel gesture is converted by the embedder itself into `ewebview_scroll()` (document coordinates are converted by the engine; see the hit test in 7.3).

## 11.5 The Listener Hooks One by One

| Hook | Semantics & typical use |
| --- | --- |
| `on_frame` | see 11.3. The only non-omittable hook |
| `on_scroll` | the engine's authoritative scroll offset (page-change zeroing / `window.scrollTo` / end of a script run). Snap the live offset and scrollbar |
| `on_url` | the visible page URL changed (an address-bar load or in-page navigation). **The embedder owns the session history** — record it here |
| `on_title` | `<title>` or `document.title`, refresh the window title |
| `on_status` | status-bar text + progress (link target, load message, `alert()` text) |
| `on_build_status` | the build overlay: `overlay=true` only during a **real build** (a post-swap script run does not count) — you may mask the page and show `text` + a 0..100 progress bar |
| `on_cursor` | requests a cursor shape when hovering a link (e.g. `"pointer"`); may be NULL/`"default"` |
| `on_dialog` | `alert/confirm/prompt` text, presented **non-blocking** (the engine never waits for a modal; confirm answers cancel, prompt answers null) |
| `on_task_start/end/failed` | subresource task lifecycle (type is `EWEB_TASK_HTML/CSS/IMAGE/SCRIPT`), drives the progress indicator |
| `on_tasks_end` | the task queue drained — the moment to turn off the loading indicator |

## 11.6 Input Mapping

```c
typedef struct eweb_event {
    int mouse_state;  /* EWEB_MOUSE_MOVE/DOWN/UP/CLICK/DOUBLE_CLICK/WHEEL */
    int button;       /* EWEB_BUTTON_NONE/LEFT/MIDDLE/RIGHT */
    int cx, cy;       /* client coordinates with the viewport top-left as origin */
    int wheel;        /* WHEEL only: -1 up / +1 down (lines) */
} eweb_event_t;
```

`cx/cy` must be computed by the embedder (the engine does not know the embedder's window geometry). The engine side handles: DOM mouse-event dispatch (capture→target→bubble, see 7.3), hover tracking, and `<a href>` click following (the 8px jitter threshold).

## 11.7 The Configuration Surface

- `ewebview_set_viewport(w, h)`: relayout + repaint + dispatch `window.onresize`;
- `ewebview_set_default_css(url)`: sets the UA default stylesheet (call before the first page loads, see 5.4). master.css is **no longer built into litehtml**; if not called the page has no UA default styles (`<div>` is not automatically block-level, etc.); sdlbrowser uses `res://html/default.css`;
- `ewebview_set_js_enabled(bool)`: on by default. Turning it off frees the VM, and the next load strips `<script>`;
- `ewebview_stop()`: aborts an in-flight load, keeping the content already on screen; `ewebview_reload()`: reloads the current page; `ewebview_get_url()`: the current visible page URL (owned by the engine, valid until the next navigation).

## 11.8 Reference Examples

### sdlbrowser (this repo `bin/sdlbrowser/`, the primary reference)

An SDL2 desktop browser shell built with the repo (`main.c`, ~1300 lines, pure C99) — the **minimal complete embedder** of engine + `port_sdl2`, and a living reference directly runnable on the host. The mapping:

| sdlbrowser | ewebview calls |
| --- | --- |
| init | `SDL_Init(VIDEO)` → `eweb_port_sdl2_set_dpr()` → `eweb_port_sdl2()` → `ewebview_create()` |
| install listeners | `cb_frame`/`cb_scroll`/`cb_url`/`cb_status`/`cb_build_status`/`cb_dialog`/`cb_task_*` |
| first screen | `ewebview_set_viewport()` + `ewebview_set_default_css("res://html/default.css")` + `ewebview_load("res://html/default.html")` |
| main loop | `ewebview_tick()` per beat; `cb_frame` adopts the new frame and parks the old one in `frame_prev`, and `upload_frame()` releases the old frame via `ewebview_release_frame()` only after the new frame is copied into the texture |
| repaint | the frame `SDL_Surface` uploaded to a streaming `SDL_Texture` → `SDL_RenderCopy` into the content area using the 11.4 displacement formula (with dpr) |
| input | SDL mouse/keyboard events → convert to client coordinates → `ewebview_post_event()`; wheel → `browser_ui_scroll()` clamp + move live offset + `ewebview_scroll()` |
| address bar/history | `on_url` records history and refreshes the address bar; forward/back/stop/reload buttons go through `ewebview_load/stop/reload` |

Link order (`bin/sdlbrowser/Makefile`, linked with `$(CXX)` to bring in libstdc++):

```
-lewebview -llitehtml -lmario_jsn -lwebp -ltinyhttpsc $(SDL2_LIBS) -lm -lpthread
# macOS extra: -framework CoreFoundation (appearance detection, AppleInterfaceStyle)
```

### WidgetWebview and xBrowser (EwokOS repo side)

**WidgetWebview** (`browser/libs/widget++/src/WidgetWebview/`) wraps the engine as a widget++ control, using `port_ewokos`:

| widget++ virtual | ewebview calls |
| --- | --- |
| ctor/dtor | `eweb_port_ewokos` + `ewebview_create` / return adopted frames + `ewebview_destroy` |
| `onTimer` | `ewebview_tick()` (driven by a 30Hz window timer) |
| `onRepaint` | blit the adopted frame (`frameGraph` zero-copy cast + the 11.4 displacement formula) |
| `onResize` | `ewebview_set_viewport()` |
| `onMouseEvent` | convert to client coordinates → `ewebview_post_event()` |
| `onScroll` | `uiLocalScroll()`: clamp → move live offset → `ewebview_scroll()` |

**xBrowser** (`browser/apps/xBrowser/`) adds application logic on top of WidgetWebview: `BrowserWidget` overrides `onTaskFailed` — when a `TASK_HTML` fails and retries are under 6, it records the URL and auto-`reload()`s about 1.5 seconds later on the 30Hz timer; `onTasksEnd` zeroes the retry count. The status bar, title bar, and address bar consume `on_status/on_title/on_url` respectively. Link order (static libraries in dependency order):

```
$(EWOK_LIB_X) -lWidgetWebview -lewebview -lmario_jsn -lwebp -llitehtml \
              -ltinyhttpsc -lsocket $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
```

`libewebview.a` comes after WidgetWebview; `eweb_port_ewokos()` is referenced by WidgetWebview, so the reference-port object file is pulled in with the library (see the link-on-demand in 10.6).

## 11.9 Embedding Checklist

- [ ] call `ewebview_tick()` on every UI tick (all callbacks happen here)
- [ ] every frame adopted in `on_frame` is eventually returned via `ewebview_release_frame()`; all cleared before `ewebview_destroy()`
- [ ] listener callbacks do not block or occupy the UI thread for long
- [ ] scroll gestures go through the two steps "local displacement + `ewebview_scroll()`", not just calling the API and waiting for a frame
- [ ] session history (the forward/back stack) is recorded in `on_url` — the engine does not manage history
- [ ] a window size change means `ewebview_set_viewport()`, otherwise layout stays at the old viewport
- [ ] use `on_task_*`/`on_build_status` to give loading feedback; use `onTaskFailed(TASK_HTML)` for retry
