# Chapter 7 · JS Bridges: DOM / Event / Web

> Language: **English** | [中文](07-js-bridges.zh.md)

The four files under `jsnative/natives/` are the **bridges** between the mario VM and the engine. They are pure C, platform-agnostic, and know nothing of litehtml — each bridge defines a callback table (`js_*_callbacks_t`) implemented by the engine (as static members in `EWebJs.cc` / `EWebCanvasGlue.cc`). JS calls a DOM method → the bridge translates it into a callback → the engine operates the litehtml document; this path is the main artery of HTML/CSS/JS cooperation.

```
JS script: document.getElementById("x").textContent = "hi"
   │
   ▼ js_dom.c native (pure C, mario object manipulation)
   js_dom_callbacks_t.get_element_by_id / el_set_text   ← callback table
   │
   ▼ EWebJs.cc: jsGetElementById / jsElSetText (engine static members)
   litehtml::document / element (engine thread, the same tree)
   │
   ▼ jsMarkLayoutDirty() → mark dirty → engine relayout → repaint a new frame
```

## 7.1 Registration Order & Shared Context

The registration order in `initJsVm()` is **mandatory** (later bridges look up classes and global objects registered by earlier ones):

```
vm_init(vm, reg_all_natives)      // mario builtin classes (Object/Array/Console/...)
js_register_dom_natives(vm, ctx, &dom_cb)   // Document/Element + window/document globals
registerCanvasNatives(vm)                   // attach getContext to the Element class
js_register_event_natives(vm)               // Event/EventTarget + on* properties
js_register_web_natives(vm, &web_cb)        // location/navigator/storage/XHR...
```

The four bridges share **the same embedder context** `ctx` (i.e. `EWebEngine*`): the DOM bridge registers first and saves it, the other bridges retrieve it via `js_dom_ctx(vm)`, avoiding repeated threading. String return values are always allocated with `mario_malloc()` and freed by the bridge with `mario_free()` after use — the contract is written in each header's comments.

## 7.2 js_dom: The Document / Element Bridge

`js_dom.h`'s `js_dom_callbacks_t` is the full set of DOM capabilities (excerpt):

| Callback | Serves the JS API |
| --- | --- |
| `alert` / `document_write` | `alert()`, `document.write/writeln` |
| `get_title`/`set_title`/`get_url` | `document.title`, reading `window.location.href` |
| `get_element_by_id` | `document.getElementById` |
| `el_get/set_text`, `el_get/set_html`, `el_get/set_attr`, `el_get_tag`, `el_remove_attr` | `textContent/innerText/innerHTML`, the attribute family, `tagName` |
| `el_is_live` | the handle-liveness gate (see 6.6) |
| `get_root`/`get_body`/`get_head` | `documentElement/body/head` |
| `query_all` | `querySelector(All)`, `getElementsBy*`, `matches/closest` (skip/max paging protocol, at most `JS_DOM_QUERY_CHUNK=32` at a time) |
| `create_element`/`create_text_node` | `document.createElement/createTextNode` |
| `el_parent/child_count/child/is_tag` | tree traversal (`children` vs `childNodes` distinguished by is_tag) |
| `el_append_child/insert_before/remove_child` | tree add/remove (layout invalidation is the engine's job) |
| `el_get_rect`/`el_get_style`/`el_focus`/`el_scroll_into_view` | geometry, computed style, focus |

**Element wrapper cache**: the bridge caches a JS wrapper object per element handle (the same node queried multiple times yields the same object); on document rebuild `js_dom_reset_element_cache()` is mandatory, otherwise a wrapper holds a handle to freed memory.

**Timers**: `setTimeout/setInterval/requestAnimationFrame` are implemented by the bridge's internal timer table; the bridge itself carries no clock — the engine passes `tic_ms()` into `js_dom_poll_timers(vm, now_ms)` each beat, and the bridge fires due callbacks by increment and returns the count fired (>0 and the engine marks dirty and repaints).

**document.write**: write content first accumulates in the bridge's buffer; after the script exits the engine takes it via `js_dom_take_write_buffer()` and reparses (see 6.5).

## 7.3 js_event: The Event / EventTarget Bridge

`js_event.h` provides the full event model: `Event/CustomEvent/MouseEvent/KeyboardEvent` constructors, all fields (`clientX/pageX/screenX/offsetX/button/buttons/keyCode/mods...`), `preventDefault/stop(Immediate)Propagation`, and full **capture→target→bubble** propagation. `addEventListener` supports capture and once; on\* properties (`onclick`, etc.) can be assigned on Element/Document/window; HTML inline attributes (`<div onclick="...">`) are compiled on demand and run with `this`=element and `event` bound.

**The full path of an input event** (engine side `jsDispatchMouseEvent()`):

```
UI thread: ewebview_post_event(EWEB_MOUSE_DOWN, LEFT, cx, cy)
   │  ECMD_INPUT enqueued
   ▼ engine thread
EWEB_MOUSE_* → "mousedown/mouseup/click/dblclick/mousemove" (wheel has no DOM event)
EWEB_BUTTON_* → DOM button (0 left / 1 middle / 2 right)
   │
   ▼ hit test (document coords = client + engine scroll offset; fixed elements use client coords)
root->get_element_by_point(cx+scrollX, cy+scrollY, cx, cy)
   │
   ▼ hover tracking: if the target changed, mouseout the old element first, then mouseover the new
js_event_dispatch_mouse(vm, target, type, ...)  ← full propagation happens inside the bridge
   │
   ▼ returns false (some listener called preventDefault) → the ECMD_INPUT handler skips the default action
```

**Link clicks** (`handleAnchorClick`, engine thread):

1. **8px jitter threshold**: if the displacement between press and release exceeds 8px it is treated as a drag-scroll rather than a click, and no navigation happens;
2. hit testing uses the **visible page** `m_doc` (the user clicks what is on screen, not the page under construction);
3. from the hit element, **walk up to the nearest `<a href>`** (the hit is usually a text node inside the anchor);
4. `container->on_anchor_click(href)` → the engine records `m_jsPendingNav` and sets build-abort → step 6 of the engine loop, `jsRunPendingNavigation()`, performs the actual jump. A script's `location.href = ...` also funnels into the same pending-navigation slot.

Event callbacks run in the VM on the engine thread, so they **cannot tear down the page in place** — navigation and scrolling are both recorded as pending and executed by the main loop after the script unwinds.

## 7.4 js_web: The BOM (Everything Else on window)

`js_web.h` covers all of a page's everyday expectations of `window`; the callback-table correspondence:

| Callback | JS surface |
| --- | --- |
| `confirm`/`prompt` | `confirm()` (non-blocking, defaults to cancel) / `prompt()` (defaults to null) |
| `get_viewport`/`get_screen` | `innerWidth/outerWidth`, `screen.width/colorDepth/orientation` |
| `get_scroll`/`scroll_to` | `scrollX/pageXOffset`, `scrollTo/scrollBy` |
| `navigate`/`reload`/`history_*` | `location.assign/href=`, `history.back/go/length` |
| `get_user_agent`/`get_language`/`get_platform` | `navigator.*` (built-in defaults when NULL, so feature-detection scripts don't throw) |
| `get_cookie`/`set_cookie` | `document.cookie` (connects to the CookieJar, Ch. 9) |
| `storage_load`/`storage_save` | `localStorage/sessionStorage` persistence (length-prefixed binary-safe blob) |
| `http_request` | **synchronous** XHR and `fetch()` (the Promise wraps the same callback); **the ewebview engine leaves it NULL** — synchronous blocking would deadlock with the async task queue, so all requests report a network error (see 9.6) |

Engine-side supplementary state: the two storage blobs (`m_jsLocalStorage` / `m_jsSessionStorage`) **deliberately outlive the VM** (the VM is rebuilt every navigation, storage is not lost); `scrollTo` records `m_jsScrollPending` executed by the main loop; `alert()` text is sent via `EUET_DIALOG` to the UI's status bar and **never blocks** waiting for a modal.

## 7.5 js_canvas: The Canvas 2D Bridge

`js_canvas.h`'s `js_canvas_callbacks_t` keeps **all the state and geometry** of `CanvasRenderingContext2D` (CTM, paths, arc/bezier subdivision, scanline fill, dashes, gradients, shadows) inside the bridge, sending only the final primitives to the engine in device coordinates: `fill_rect`, `draw_line`, `fill_circle`, `arc`, `round`, `stroke_quadratic/bezier`, `set/get_pixel`, `blit`, `draw_text`, `set_clip`. See Ch. 8 for details.

## 7.6 Progressive Adoption & Degradation of Bridges

**Every callback in every table is OPTIONAL**: a NULL callback makes the corresponding API degrade as a "missing feature" (returning null/""/0/false) rather than crash. `js_web` even allows the whole table to be NULL — the APIs are still installed and all report default values, so detection-style scripts (`if (window.X) ...`) don't throw. This lets a new platform's bridges also light up incrementally (the same as the porting order in Ch. 10).

## 7.7 Known Gaps

- `window.postMessage()` accepts the call but does nothing (no iframe/worker/popup);
- `globalThis` aliases `window`, not the VM's true global (`vm->root`) — reading globals works, but globals created through it are invisible to bare identifiers;
- `performance.mark()/measure()` keep no timeline;
- `crypto.getRandomValues()` is seeded from the monotonic clock, good only for ids, not keys;
- the bridge's XHR contract is always synchronous (`open()`'s async parameter is ignored), with `fetch`'s Promise wrapped on top; and the ewebview engine does not implement the `http_request` callback, so XHR/`fetch` currently **always end in a network error** (XHR `status 0` / fetch rejected) without throwing (see 9.6).
