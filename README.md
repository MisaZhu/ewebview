# ewebview

**ewebview** is a self-contained embedded Web engine (HTML + CSS + JavaScript) targeting resource-constrained operating systems and bare-metal environments. It renders web pages into an abstract ARGB8888 memory canvas (`eweb_surface_t`); all platform-specific concerns — graphics, fonts, image decoding, networking, and clock — are injected through the `eweb_port_t` callback table (HAL). The core code has no dependency on any specific OS, windowing system, or graphics library.

The reference platform in this repository is EwokOS (`ewebview/porting/src/port_ewokos.c`); the browser application xBrowser is built on top of it.

## Features

- **HTML5 / CSS**: litehtml layout engine + Google Gumbo parser (vendored under `litehtml/`), supporting common CSS2.1/3 properties, media queries, tables, and flex layout.
- **JavaScript**: mario bytecode virtual machine (`mario_js/` submodule), covering the ES5 core plus a large set of ES6+ features; four pure-C bridge layers for DOM / Event / BOM / Canvas 2D (`jsnative/`).
- **Canvas 2D**: full `CanvasRenderingContext2D` (paths, gradients, patterns, dashed lines, shadows, ImageData, drawImage, Path2D).
- **Networking**: http/https (BearSSL), `file://`, and a private `res://` resource protocol; redirects are followed hop-by-hop by the core with per-hop Cookie re-resolution.
- **Multi-threaded pipeline**: engine thread + on-demand download thread, with zero blocking on the UI thread; frame-pool ownership transfer + back-pressure achieve zero pixel copies across threads.
- **Portable**: all platform dependencies converge into 6 callback tables (gfx / font / image / net / clock / sys), of which only gfx + font + clock are required — the rest can be filled in incrementally.

## Directory Layout

```
ewebview/
├── Makefile          # Top-level build: DIRS = libwebp litehtml jsnative ewebview
├── ewebview/         # Engine core (libewebview.a)
│   ├── include/ewebview.h           # Public C API
│   ├── src/                         # Platform-independent C++14 core
│   │   ├── ewebview.cc              #   C API, engine thread, build state machine, frame pool
│   │   ├── EWebContainer.{h,cc}     #   litehtml document_container implementation
│   │   ├── EWebJs.cc                #   mario VM lifecycle + DOM/Event/Web bridge callbacks
│   │   ├── EWebCanvas.{h,cc}        #   <canvas> offscreen surface and drawing primitives
│   │   ├── EWebCanvasGlue.cc        #   Canvas bridge trampolines + canvas registry
│   │   ├── EWebCookies.{h,cc}       #   Process-wide CookieJar (SameSite isolation)
│   │   └── eweb_el_input.{h,cc}     #   <input> replaced element
│   └── porting/                     # Platform porting layer (HAL)
│       ├── include/ewebview_port.h  #   Contract of the six eweb_port_t callback tables
│       └── src/port_ewokos.c        #   EwokOS reference implementation (graph/font/tinyhttpsc)
├── jsnative/         # JS native bridges (linked into libmario.a): js_dom/js_event/js_web/js_canvas
├── mario_js/         # mario bytecode VM submodule (core + JS frontend + built-in class library)
├── litehtml/         # litehtml + gumbo (liblitehtml.a), with host-side diagnostics under hosttest/
├── libwebp/          # WebP decoding (libwebp.a)
└── docs/wiki/        # Chaptered architecture documentation for this engine (see below)
```

## Build

Depends on the EwokOS source tree (defaults to this repository root; override with `ewokos=/path`):

```sh
make                    # Builds libwebp → litehtml → jsnative → ewebview in order
make clean
```

Artifacts are installed into the EwokOS SDK directory `system/build_aarch64/virt/`:

| Artifact | Description |
| --- | --- |
| `lib/libewebview.a` | Engine core (C++14) + reference porting layer (C99, linked on demand) |
| `lib/liblitehtml.a` | litehtml + gumbo |
| `lib/libmario.a`    | mario VM + JS built-in classes + four browser bridges |
| `lib/libwebp.a`     | WebP decoding |
| `include/ewebview.h`, `include/ewebview_port.h`, `include/mario/*.h` | Public headers |

Application-side link order (e.g. `browser/apps/xBrowser`): `-lewebview -lmario -lwebp -llitehtml` plus the EwokOS graphics and libc library groups.

## Minimal Embedding Example

```c
#include <ewebview.h>
#include <ewebview_port.h>

eweb_port_t port;
eweb_port_ewokos(&port, NULL);              /* or fill in your own platform tables */
ewebview_t* v = ewebview_create(&port);

static eweb_listener_t lis;                 /* on_frame/on_scroll/on_url/... */
eweb_listener_init(&lis);
lis.on_frame = my_on_frame;                 /* claim the frame: release after blit */
ewebview_set_listener(v, &lis);

ewebview_set_viewport(v, 800, 480);
ewebview_load(v, "https://www.w3.org");

/* UI main loop: forward input and scroll; call ewebview_tick() once per frame */
ewebview_tick(v);
```

For the full embedding flow, scroll model, and porting guide, see the documentation.

## Documentation

- **[docs/wiki/](docs/wiki/README.md)** — Chaptered architecture docs: overall architecture, threading model, page pipeline, HTML/CSS/JS interplay, Canvas, networking/cookies, **platform porting guide**, and embedding guide.
- [mario_js/docs/wiki/](mario_js/docs/wiki/README.md) — Design docs for the mario virtual machine itself (bytecode, compiler, GC, native extensions).

## License

See [LICENSE](LICENSE) (Apache License 2.0). litehtml/gumbo, mario_js, and libwebp are vendored copies under their respective upstream licenses.
