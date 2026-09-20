<div align="center">
  <h1>ewebview</h1>
  <p>A self-contained embedded Web engine for resource-constrained systems and bare-metal environments.</p>
  <p><a href="README.md">English</a> | <a href="README.zh-CN.md">简体中文</a></p>
  <p>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-blue.svg" alt="License: Apache-2.0"></a>
    <a href="https://github.com/MisaZhu/ewebview/actions/workflows/ci.yml"><img src="https://github.com/MisaZhu/ewebview/actions/workflows/ci.yml/badge.svg?branch=main&job=build%20%28macos-14%29" alt="CI macOS"></a>
    <a href="https://github.com/MisaZhu/ewebview/actions/workflows/ci.yml"><img src="https://github.com/MisaZhu/ewebview/actions/workflows/ci.yml/badge.svg?branch=main&job=build%20%28ubuntu-latest%29" alt="CI Linux"></a>
  </p>
</div>

**ewebview** is a self-contained embedded Web engine (HTML + CSS + JavaScript) targeting resource-constrained operating systems and bare-metal environments. It renders web pages into an abstract ARGB8888 memory canvas (`eweb_surface_t`); all platform-specific concerns — graphics, fonts, image decoding, networking, and clock — are injected through the `eweb_port_t` callback table (HAL). The core code has no dependency on any specific OS, windowing system, or graphics library.

Two reference ports ship in-tree:

- **`sdl2`** (default): a desktop port built on SDL2 / SDL2_ttf / SDL2_image / SDL2_gfx + libtinyhttpsc, exercised by the bundled `bin/sdlbrowser` shell — builds and runs natively on macOS / Linux.
- **`ewokos`**: the original EwokOS port (`graph` / `font` / `vfs` / `x` / libtinyhttpsc), which the xBrowser application is built on top of.

## Features

- **HTML5 / CSS**: litehtml layout engine + Google Gumbo parser (vendored under `litehtml/`), supporting common CSS2.1/3 properties, media queries, tables, and flex layout.
- **JavaScript**: mario bytecode virtual machine (`mario_js/` submodule), covering the ES5 core plus a large set of ES6+ features; four pure-C bridge layers for DOM / Event / BOM / Canvas 2D (`jsnative/`). Both inline `<script>` bodies and external `<script src="...">` are supported (the latter is fetched as `EWEB_TASK_SCRIPT` and spliced into document order).
- **Canvas 2D**: full `CanvasRenderingContext2D` (paths, gradients, patterns, dashed lines, shadows, ImageData, drawImage, Path2D).
- **Networking**: http/https via the bundled `libtinyhttpsc` (BearSSL), plus `file://` and a private `res://` resource protocol; redirects are followed hop-by-hop by the core with per-hop Cookie re-resolution.
- **Multi-threaded pipeline**: engine thread + on-demand download thread, with zero blocking on the UI thread; frame-pool ownership transfer + back-pressure achieve zero pixel copies across threads.
- **Portable**: all platform dependencies converge into 6 callback tables (gfx / font / image / net / clock / sys), of which only gfx + font + clock are required — the rest can be filled in incrementally.
- **HiDPI ready**: the SDL2 port exposes `eweb_port_sdl2_set_dpr()` so layout stays in CSS pixels while rasterisation runs at native device resolution.

## Directory Layout

```
ewebview/
├── Makefile          # Top-level build: DIRS = libtinyhttpsc libwebp jsnative litehtml ewebview
│                     # (+ bin/sdlbrowser when PORTING=sdl2, the default)
├── make.inc          # Shared build config: PORTING / OS_TYPE / ARCH / HW / SDK_DIR
├── host.rule         # Host (macOS/Linux, native gcc) toolchain rules
├── libtinyhttpsc/    # Portable BearSSL HTTP/HTTPS client        -> libtinyhttpsc.a
├── libwebp/          # WebP decoding                             -> libwebp.a
├── jsnative/         # mario VM + JS built-ins + browser bridges -> libmario_jsn.a
│   └── natives/      #   js_dom / js_event / js_web / js_canvas (pure C)
├── mario_js/         # mario bytecode VM submodule (core + JS frontend + built-ins)
├── litehtml/         # litehtml + gumbo                          -> liblitehtml.a
├── ewebview/         # Engine core + reference ports             -> libewebview.a
│   ├── include/ewebview.h           # Public C API
│   ├── src/                         # Platform-independent C++14 core
│   │   ├── ewebview.cc              #   C API, engine thread, build state machine, frame pool
│   │   ├── EWebContainer.{h,cc}     #   litehtml document_container implementation
│   │   ├── EWebJs.cc                #   mario VM lifecycle + DOM/Event/Web bridge callbacks
│   │   ├── EWebCanvas.{h,cc}        #   <canvas> offscreen surface and drawing primitives
│   │   ├── EWebCanvasGlue.cc        #   Canvas bridge trampolines + canvas registry
│   │   ├── EWebCookies.{h,cc}       #   Process-wide CookieJar (SameSite isolation)
│   │   ├── EWebInternal.h           #   Internal engine state + tuning constants
│   │   ├── EWebLog.h                #   Logging macro (compiled out unless -DEWEBVIEW_DEBUG)
│   │   └── eweb_el_input.{h,cc}     #   <input> replaced element
│   └── porting/                     # Platform porting layer (HAL)
│       ├── include/ewebview_port.h  #   Contract of the six eweb_port_t callback tables
│       └── src/
│           ├── sdl2/port_sdl2.c     #   Desktop SDL2 reference port (default)
│           └── ewokos/port_ewokos.c #   EwokOS reference port
├── bin/sdlbrowser/   # SDL2 desktop browser shell around libewebview -> sdlbrowser
│   └── res/html/     #   Bundled default.html / default.css / master.css served via res://
└── docs/wiki/        # Chaptered architecture documentation (see below)
```

## Build

Two build modes, selected by `OS_TYPE`:

- **Host build** (default, `OS_TYPE` unset): compiles natively with `gcc` / `g++` (or `ccache`-wrapped) on macOS / Linux. Artifacts land in `build_$(ARCH)/$(HW)/` inside this repository — the default is `build_aarch64/virt/`.
- **EwokOS cross build** (`OS_TYPE=ewokos`): compiles against the EwokOS SDK. Artifacts are installed into `$(ewokos)/system/build_$(ARCH)/$(HW)/`. The EwokOS root defaults to this repository's parent directory; override with `make ewokos=/path/to/ewokos`.

`PORTING` picks the reference port compiled into `libewebview.a`: `sdl2` (default) or `ewokos`. Only the selected port object is archived, and it is pulled in on demand at link time (a foreign embedder that supplies its own port never drags SDL2 / graph symbols in).

```sh
make                                        # host build, PORTING=sdl2 (default)
make PORTING=ewokos OS_TYPE=ewokos          # EwokOS cross build
make clean
```

Build order (top-level `Makefile`):

```
libtinyhttpsc → libwebp → jsnative → litehtml → ewebview  (+ bin/sdlbrowser for PORTING=sdl2)
```

Artifacts installed into `$(SDK_DIR)/lib` and `$(SDK_DIR)/include`:

| Artifact | Description |
| --- | --- |
| `lib/libewebview.a`   | Engine core (C++14) + the selected reference port (C99, linked on demand) |
| `lib/liblitehtml.a`   | litehtml + gumbo |
| `lib/libmario_jsn.a`  | mario VM + JS built-in classes + four browser bridges |
| `lib/libwebp.a`       | WebP decoding |
| `lib/libtinyhttpsc.a` | BearSSL HTTP/HTTPS client (shared by both reference ports) |
| `include/ewebview.h`, `include/ewebview_port.h` | Public engine headers |
| `include/mario/*.h`   | mario VM + bridge headers (`mario.h`, `js_dom.h`, `js_event.h`, `js_web.h`, `js_canvas.h`) |
| `include/litehtml/`, `include/gumbo/`, `include/litehtml.h` | litehtml + gumbo headers |
| `include/tinyhttpsc/`, `include/webp.h` | Networking / image headers |
| `bin/sdlbrowser`, `bin/res/` | SDL2 demo shell + its bundled `res://` assets (host build only) |

Application-side link order: `-lewebview -llitehtml -lmario_jsn -lwebp -ltinyhttpsc` plus the platform's graphics/libc group. `bin/sdlbrowser/Makefile` shows the full desktop recipe (SDL2 + companions via `pkg-config`).

## Running the SDL2 demo

The host build produces `build_aarch64/virt/bin/sdlbrowser`. It ships with a bundled `res://` root (`bin/res/`) containing a start page, a demo stylesheet, and a `master.css`, so it works out of the box:

```sh
./build_aarch64/virt/bin/sdlbrowser                       # loads res://html/default.html
./build_aarch64/virt/bin/sdlbrowser https://www.w3.org    # or any URL
```

Requires SDL2 / SDL2_ttf / SDL2_image / SDL2_gfx at runtime (`brew install sdl2 sdl2_ttf sdl2_image sdl2_gfx` on macOS; distro packages on Linux).

## Minimal Embedding Example

```c
#include <ewebview.h>
#include <ewebview_port.h>

/* SDL2 port is not declared in the public header; declare it yourself. */
extern void eweb_port_sdl2(eweb_port_t* port, void* ud);

eweb_port_t port;
eweb_port_init(&port);
eweb_port_sdl2(&port, NULL);                /* or eweb_port_ewokos / your own tables */
ewebview_t* v = ewebview_create(&port);

static eweb_listener_t lis;                 /* on_frame / on_scroll / on_url / ... */
eweb_listener_init(&lis);
lis.on_frame = my_on_frame;                 /* claim the frame: release after blit */
ewebview_set_listener(v, &lis);

ewebview_set_viewport(v, 800, 480);
ewebview_set_default_css(v, "res://html/master.css");   /* optional UA stylesheet */
ewebview_load(v, "https://www.w3.org");

/* UI main loop: forward input and scroll; call ewebview_tick() once per frame */
ewebview_tick(v);
```

For the full embedding flow, scroll model, and porting guide, see the documentation.

## Documentation

- **[docs/wiki/](docs/wiki/README.md)** — Chaptered architecture docs: overall architecture, threading model, page pipeline, HTML/CSS/JS interplay, Canvas, networking/cookies, **platform porting guide**, and embedding guide.
- [mario_js/docs/wiki/](mario_js/docs/wiki/README.md) — Design docs for the mario virtual machine itself (bytecode, compiler, GC, native extensions).

## License

See [LICENSE](LICENSE) (Apache License 2.0). litehtml/gumbo, mario_js, libwebp, and libtinyhttpsc (BearSSL) are vendored copies under their respective upstream licenses.
