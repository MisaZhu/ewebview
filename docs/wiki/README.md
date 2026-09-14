# ewebview Architecture Wiki

> Language: **English** | [中文](README.zh.md)

Welcome to the design documentation for **ewebview**. This Wiki is written for readers who need to understand, embed, or port the engine, walking from the overall architecture all the way through how HTML/CSS/JS cooperate, the threading model, and platform porting.

ewebview is a self-contained embedded web engine: its core only knows an abstract ARGB8888 in-memory canvas and a set of platform callback tables (`eweb_port_t`) — it knows nothing about any specific operating system. HTML layout (litehtml + gumbo), JavaScript (the mario VM), and Canvas 2D are all internal engine components; graphics, fonts, image decoding, networking, and the clock are all injected by the "porting layer".

The repository ships two reference ports: **`sdl2`** (default, desktop, with the `bin/sdlbrowser` demo shell) and **`ewokos`** (EwokOS, on which xBrowser is built).

---

## Reading Order

Reading in order is recommended; each chapter builds on the concepts of the previous one:

| Chapter | Title | Summary |
| --- | --- | --- |
| Ch. 1 | [Overall Architecture](01-overview.md) | What the engine is, the layered structure, the module map, the data flow of one page load |
| Ch. 2 | [Directory Layout & Build System](02-build.md) | Build order of the five sub-libraries, host/cross build modes, SDK install, link order, debug switches |
| Ch. 3 | [Threading Model & Frame Delivery](03-threading.md) | Engine thread / download worker pool / UI thread, command and event queues, frame pool and backpressure |
| Ch. 4 | [Page Load Pipeline](04-page-pipeline.md) | The six stages of the build state machine, build abort, style sharding, progressive painting |
| Ch. 5 | [HTML & CSS: Parsing, Layout, and the Container](05-html-css.md) | How gumbo → litehtml → document_container callbacks map onto the porting layer |
| Ch. 6 | [JavaScript: mario VM Integration & Script Scheduling](06-js-vm.md) | Script extraction, VM lifecycle, the run-budget watchdog, document.write re-parsing |
| Ch. 7 | [JS Bridges: DOM / Event / Web](07-js-bridges.md) | The four pure-C bridges, element-handle liveness, event propagation, the BOM surface |
| Ch. 8 | [Canvas 2D](08-canvas.md) | The in-bridge geometry state machine, the offscreen canvas registry, compositing back to the page |
| Ch. 9 | [Networking, Cookies & Storage](09-network-cookies.md) | The subresource task queue, redirects and cookie scoping, localStorage |
| Ch. 10 | [Platform Porting Guide](10-porting.md) | The six `eweb_port_t` tables in detail, REQUIRED/OPTIONAL, an ewokos/sdl2 reference-port comparison, steps to land a new platform |
| Ch. 11 | [Embedding Guide](11-embedding.md) | C API usage, listeners, the scrolling model, sdlbrowser/WidgetWebview/xBrowser examples |

---

## ewebview in One Minute

```
   URL (http/https/file/res)
        │
        ▼  Download worker pool: net.request / net.read_file / image.decode   ── Ch. 9
  HTML byte stream + CSS + images
        │
        ▼  gumbo parse → litehtml document tree + stylesheets + layout     ── Ch. 5
  litehtml::document (element tree)
        │
        ▼  <script> (inline + external src download) handed to the mario VM; DOM/Event/Web/Canvas bridges write back to the document ── Ch. 6, 7, 8
  Laid-out document + canvas bitmaps
        │
        ▼  Engine thread rasterizes onto frame-pool surfaces via gfx/font callbacks   ── Ch. 3
  ARGB8888 frame (eweb_surface_t*, i.e. graph_t* on EwokOS, wrapping SDL_Surface* on SDL2)
        │
        ▼  on_frame() hands it to the embedder to blit into their own window  ── Ch. 11
   SCREEN
```

Every platform-specific capability enters the engine from the **`eweb_port_t` porting tables** (Ch. 10) on the right.
