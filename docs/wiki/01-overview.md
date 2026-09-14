# Chapter 1 · Overall Architecture

> Language: **English** | [中文](01-overview.zh.md)

## 1.1 What ewebview Is

ewebview is a **self-contained embedded web engine**: give it a URL and, on background threads, it downloads, parses, lays out, runs scripts, and rasterizes, handing the result to the embedder as an ARGB8888 in-memory canvas (`eweb_surface_t`). The embedder only needs to blit that canvas into their own window.

Its fundamental difference from Chromium/WebKit-class engines lies in the **direction of dependency**:

```
Traditional browser: engine -> calls OS graphics/network/font APIs directly
ewebview:            engine -> eweb_port_t callback tables -> the "porting layer" decides what to call
```

The core code (`ewebview/src/`) **contains no platform symbols** — no `graph_t`, no sockets, no filesystem calls. It only knows:

- `eweb_surface_t*`: an opaque canvas handle (on EwokOS it happens to be `graph_t*`; on SDL2 it is a `sdl_surf_t*` wrapping an `SDL_Surface`);
- `eweb_font_t*`: an opaque font handle (on EwokOS `font_t*`; on SDL2 a `sdl_font_t*` caching `TTF_Font*` by pixel size);
- the six function tables in `eweb_port_t`: gfx / font / image / net / clock / sys.

This contract is declared in [`ewebview/porting/include/ewebview_port.h`](../../ewebview/porting/include/ewebview_port.h); the public API is in [`ewebview/include/ewebview.h`](../../ewebview/include/ewebview.h).

## 1.2 Design Goals

1. **Portable**: switching platforms = writing a new port (filling the six tables), with zero changes to the core. The repository provides two reference implementations: `port_ewokos.c` (~400 lines, the native EwokOS graphics stack) and `port_sdl2.c` (~1400 lines, desktop SDL2 / SDL2_ttf / SDL2_image / SDL2_gfx + libtinyhttpsc, with HiDPI).
2. **Incremental**: every table except gfx/font/clock is OPTIONAL; leaving one empty degrades the corresponding capability rather than crashing — you can light up layout first, then add networking, then images.
3. **The UI never blocks**: download, parse, layout, and scripts all run on the engine and download threads; the UI thread only posts commands, receives events, and blits frames.
4. **Zero-copy frame delivery**: frame surfaces cross threads by ownership transfer; the embedder may even cast the surface back to the platform's native type and blit it directly (`graph_t*` on EwokOS, `SDL_Surface*` on SDL2).
5. **Built for constrained environments**: everything statically linked, no C++ exception/RTTI dependency (litehtml builds with `-fno-rtti`), and script execution carries a watchdog so a malicious infinite loop cannot lock up the engine.

## 1.3 Layered Structure

```
┌────────────────────────────────────────────────────────────┐
│ Embedder: sdlbrowser / xBrowser / WidgetWebview             │  UI thread
├────────────────────────────────────────────────────────────┤
│ Public C API: ewebview.h                                    │
│   create/load/stop/reload/set_viewport/post_event/scroll/   │
│   tick/release_frame + eweb_listener_t callback table       │
├────────────────────────────────────────────────────────────┤
│ Engine core (libewebview.a, platform-agnostic C++14)         │  engine thread
│   ewebview.cc        engine loop, build state machine,       │  download thread
│                      frame pool, download scheduling         │
│   EWebContainer.cc   litehtml document_container impl        │
│   EWebJs.cc          mario VM lifecycle + DOM/Event/Web       │
│                      bridge callbacks                         │
│   EWebCanvas(Glue)   <canvas> offscreen surfaces + Canvas     │
│                      bridge glue                             │
│   EWebCookies.cc     process-level CookieJar                 │
├───────────────┬──────────────────────────┬─────────────────┤
│ litehtml+gumbo│ mario VM + jsnative       │ libwebp          │
│ (liblitehtml.a│ bridges (libmario_jsn.a)  │ (libwebp.a)      │
├───────────────┴──────────────────────────┴─────────────────┤
│ Porting HAL: eweb_port_t (gfx/font/image/net/clock/sys)      │  platform
│   Reference impls: port_sdl2.c   = SDL2/SDL2_ttf/SDL2_image/  │
│                                    SDL2_gfx                  │
│                    port_ewokos.c = graph/font/x/vfs          │
│   Both share libtinyhttpsc (BearSSL HTTP/HTTPS) as the net    │
│   table backend                                              │
└────────────────────────────────────────────────────────────┘
```

- **litehtml + gumbo** (vendored in `litehtml/`): the HTML parsing and CSS layout engine. gumbo is the HTML5 parser (C); litehtml builds the element tree on top of it and performs style computation and layout (C++).
- **mario VM** (the `mario_js/` submodule): a bytecode virtual machine + JavaScript frontend, pure C, no dependencies; memory allocation and output are injected by the host.
- **jsnative** (`jsnative/natives/`): the four browser bridges — `js_dom` (Document/Element), `js_event` (Event/EventTarget), `js_web` (window/location/storage/XHR/fetch), `js_canvas` (Canvas 2D) — likewise pure C and platform-agnostic. Archived together with the VM as `libmario_jsn.a`.
- **libtinyhttpsc** (`libtinyhttpsc/`): a standalone BearSSL HTTP/HTTPS client (pure C, no OS dependency); the `net.request` of both reference ports connects to it. Archived as `libtinyhttpsc.a`.
- **libwebp**: WebP decoding; other image formats (png/jpeg/gif/tga/svg) are wired up by the porting layer itself (EwokOS goes through graph_image/plutosvg, SDL2 through SDL2_image).

## 1.4 Core Internal Module Map

The engine core exposes only one opaque external type `ewebview_t`, whose internals are `eweb::EWebEngine` (see `src/EWebInternal.h`):

| Member/Module | Responsibility |
| --- | --- |
| `m_port` | The `eweb_port_t` copied by value at creation; the entry point for every platform call |
| `m_doc` / `m_container` | The litehtml document and container of the currently **visible page** |
| `m_buildDoc` / `m_buildContainer` / `m_buildPhase` | The new page **under construction** and the build state machine |
| `m_browser_context` / `m_buildContext` | Two litehtml contexts (stylesheet caches), used alternately as a double buffer |
| `m_cmdQueue` / `m_uiQueue` | UI→engine command queue, engine→UI event queue |
| `m_taskQueue` / `m_resultQueue` | Subresource download task and result queues (download thread) |
| `m_freeFrames` / `m_pendingFrame` | The viewport frame pool (double buffer) and the frame awaiting claim |
| `m_jsVm` + JS state | mario VM, script lists (`m_jsScripts` / external `m_jsScriptSrcs`), run budget, mutation log |
| `m_jsCanvases` | The offscreen-canvas registry for each `<canvas>` |
| `EWebCookieJar` (process singleton) | The cookie store shared by HTTP and document.cookie |

Two key internal interfaces decouple the core:

- **`EWebContainerHost`** (`EWebContainer.h`): the things the container needs the engine to do during litehtml parsing/layout — queue image tasks, load CSS, record media, queue navigation, query the build-abort flag. The container therefore does not directly reference engine types.
- **`js_*_callbacks_t`** (`jsnative/natives/*.h`): the four JS bridges' callback tables into the engine. The bridges are pure C and know nothing of litehtml; static member functions in `EWebJs.cc`/`EWebCanvasGlue.cc` land these callbacks onto litehtml documents and canvases.

## 1.5 Data Flow of One Page Load

```
ewebview_load(url)                       [UI thread]
   │  posts ECMD_NAVIGATE (returns immediately)
   ▼
engineLoop() takes the command → engineNavigate()   [engine thread]
   │  queues the main-document task EWEB_TASK_HTML
   ▼
taskLoop() takes the task                [download thread]
   │  net.request() fetches bytes, cookies handled per hop, images decoded along the way
   ▼
pushResult() → engine processResults()
   │  HTML arrives: extract_scripts() strips inline script bodies;
   │            external <script src> queued in document order as EWEB_TASK_SCRIPT downloads
   ▼
build state machine (detailed in Ch. 4):
   PRELOAD_CSS → CREATE_DOC (gumbo+litehtml parse) → RUN_JS (optional)
   → RENDER_DOC (style+layout) → SWAP_DOC (new page on screen) → remaining scripts run progressively
   │
   ▼
engineRenderFrame(): gfx/font callbacks rasterize the viewport → EUET_FRAME
   │
   ▼
ewebview_tick() → listener.on_frame(frame)   [UI thread]
   embedder blits to screen; the old frame returns to the pool via ewebview_release_frame()
```

Thereafter the page enters its **live phase**: JS timers (`jsPollTimers`), mouse events (`jsDispatchMouseEvent` → DOM event propagation → possibly mutating the DOM → marking dirty → relayout → repaint a new frame), scrolling, and `<canvas>` animation are all continuously driven by the engine loop; the UI thread just calls `ewebview_tick()` once per tick to collect events.

## 1.6 HTML, CSS, JS Cooperation (Overview)

The division of labor and meeting points among the three (expanded in later chapters):

- **HTML (gumbo/litehtml)** produces the element tree; `<script>` is stripped out by `extract_scripts()` before parsing and handed to JS, while `<link>`/`<style>` are passed by the container to the CSS subsystem.
- **CSS (litehtml stylesheet + master.css)** decides how the element tree is laid out and painted; painting itself does not touch pixels directly, but calls back the container's `draw_text`/`draw_background`/`draw_borders`, which then land on the canvas via `eweb_port_t.gfx/font`.
- **JS (mario VM + four bridges)** reads and writes **the same litehtml element tree** through the DOM bridge (an element handle is a `litehtml::element*`), receives input events through the Event bridge, reads/writes cookies/storage and initiates navigation through the Web bridge, and draws into `<canvas>` bitmaps through the Canvas bridge. Every DOM change by JS marks the layout dirty, and the engine relayouts and repaints at the appropriate moment — this is the complete chain behind "change the DOM and the screen changes".

Details: Ch. 5 (HTML/CSS), Ch. 6 (JS scheduling), Ch. 7 (JS bridges).

## 1.7 Relationship to Other Components

This repository is a **standalone** engine repo. Its artifacts (static libraries + public headers) are installed into an SDK directory (`build_$(ARCH)/$(HW)/`, or `system/build_$(ARCH)/$(HW)/` for EwokOS) and consumed by downstream applications:

- **`bin/sdlbrowser`** (in this repo): an SDL2 desktop browser shell — the minimal complete embedder of the engine + `port_sdl2`, with address bar / status bar / history / scroll wheel / HiDPI. It is a living reference for porting and embedding (Ch. 11).
- **WidgetWebview** (EwokOS repo `browser/libs/widget++/src/WidgetWebview/`): a reference embedder wrapping ewebview as a widget++ control, ~500 lines — proving that no web logic beyond the core is needed.
- **xBrowser** (EwokOS repo `browser/apps/xBrowser/`): a full browser application = WidgetWebview + address bar/status bar/history, using `port_ewokos`.
- Test pages: `bin/sdlbrowser/res/html/` ships a starter page and `master.css`; more complete DOM / event / form / timer / canvas / storage test-case pages live in the EwokOS repo's `browser/data/test/html/`.
