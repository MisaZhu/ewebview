# Chapter 10 · Platform Porting Guide

> Language: **English** | [中文](10-porting.zh.md)

The ewebview core knows no operating system: it only renders pages into an abstract ARGB8888 in-memory canvas (`eweb_surface_t`), with all platform capabilities injected via **`eweb_port_t`** (`porting/include/ewebview_port.h`). Porting a platform = filling six callback tables. This chapter explains each table's contract, then gives landing steps against the reference port `porting/src/ewokos/port_ewokos.c`.

## 10.1 Design Rules of the Porting Layer

- **`eweb_port_t` is a value bundle of six tables**: `gfx / font / image / net / clock / sys`, **copied by value** at `ewebview_create()`; the caller may then free its own copy.
- **Each table carries its own `ud` pointer**, passed back verbatim to every callback — the porting layer uses it to dispatch to its own context; the core never interprets it.
- **Opaque handles**: `eweb_surface_t` / `eweb_font_t` are opaque to the core; the concrete types are up to the port (EwokOS: `graph_t*` / `font_t*`). If the embedder **is** the porting layer, it can get the concrete pointer back via `surface_native()` for zero-copy on-screen blitting (this is how widget++ blits frames into its window, see Ch. 11).
- **Colors are always `0xAARRGGBB`**, coordinates are always integer device pixels (except where a prototype notes float).
- **Every callback is OPTIONAL unless marked REQUIRED**: a NULL hook makes the corresponding capability **degrade** rather than crash — a port can light up incrementally.

## 10.2 The Six Tables Overview & Threading Matrix

| Table | Calling thread | REQUIRED members | When the whole table is missing |
| --- | --- | --- | --- |
| `gfx` | **engine thread only** | `surface_new` / `surface_free` / `surface_dims` / `blit` / `blit_fit_alpha` | cannot render |
| `font` | **engine thread only** | `create` / `destroy` / `metrics` / `char_width` / `draw_text` | cannot lay out text |
| `image` | **download thread** | none (only REQUIRED if images are needed) | `<img>`/background images stay 0×0 |
| `net` | **download thread only** | `request`+`free_response` (http pages), `read_file` (file pages) | no network/local-file loading |
| `clock` | **called by both engine + download threads** | `tic_ms` | timers/debouncing/budgets all fail |
| `sys` | engine thread | none | logs discarded, handle checking reduced to the liveness flag |

The minimum usable set = **gfx + font + clock** (a page can be laid out and painted); the rest is added incrementally as needed.

## 10.3 The gfx Table: Surfaces & Primitives

**Surface lifecycle** (REQUIRED): `surface_new(w,h)` allocates a canvas (used by the frame pool, `<canvas>` backing stores, and anonymous bitmaps); `surface_free` must tolerate NULL; `surface_dims` validates size before frame-pool reuse.

**Direct pixel access**: `surface_pixels` returns a `uint32_t` buffer of `w*h` — Canvas ImageData reads/writes take this fast path; it **may return NULL**, and the core automatically falls back to `get_pixel`/`set_pixel`. `surface_native` returns the platform's native handle (the core never dereferences it).

**Primitive groups**: rect/line (`fill_rect`/`rect`/`line`/`wline`), circles and arcs (`circle`/`fill_circle`/`arc`/`fill_arc`, angles in radians, clockwise from +x), rounded rects (`round`/`fill_round`), curve strokes (`stroke_quadratic`/`stroke_bezier`), pixels (`set_pixel`/`get_pixel`), blits (`blit` 1:1 copy; `blit_fit_alpha` scale + 0..255 alpha blend — used by both background images and Canvas compositing).

**Curve subdivision** (OPTIONAL): `flatten_quadratic`/`flatten_cubic` write the de Casteljau subdivision result as an interleaved float polyline (excluding the start point), for the bridge to scanline-**fill** curves (see 8.3). Left NULL, the core degrades to a straight chord. `surface_set_clip`/`surface_unset_clip` are rectangular clipping (both `border-radius` and Canvas `clip()` depend on them).

## 10.4 The font Table: Handles Carry No Size

- `create(family)` returns a font handle; **size is not baked into the handle**, and every measure/draw call passes `size` — one handle serves all sizes. The CSS family is only a hint (the reference ports ignore it, always `font_new("system-cn")`).
- `metrics(f, size)` back-fills ascent/descent/height/x_height — all of litehtml's layout line heights come from here.
- `char_width(f, size, codepoint)` (a UTF-32 code point) is **the hottest callback in all of layout**; the engine side has an 8192-slot char-width cache as a backstop (see 5.3), but the port implementation itself should also be as close to an O(1) table lookup as possible.
- `text_size` (OPTIONAL): whole-string measurement; when absent the core sums `char_width` per code point.
- `draw_text(s, x, y, utf8, f, size, color)`: top-left baseline positioning.

## 10.5 image / net / clock / sys

**image.decode(data, size)**: decodes png/jpeg/gif/svg/webp/... bytes into a **new** ARGB8888 surface (returns NULL on failure; the core frees it with `gfx.surface_free`). It runs on the **download thread** (pure heap work) and must not touch engine-thread-exclusive state.

**The net table**:

| Member | Contract |
| --- | --- |
| `request` | issues **one** http(s) request and **must not auto-follow redirects** (the core follows hop by hop itself to recompute cookie scope, see 9.2). `req_headers` carries the Cookie header the core built for this hop. Returns true and fills `resp` upon receiving a response (even 4xx/5xx); returns false on a transport-layer failure |
| `free_response` | frees `resp`'s body/headers/native; REQUIRED paired with `request`. The response memory stays valid until this call |
| `read_file` | `file://`: reads a file by absolute path, returns a **malloc'd** buffer (the core `free()`s it) |
| `resolve_resource` | OPTIONAL: resolves a port-private scheme (EwokOS's `res://`) into a real path |

**The clock table**: `tic_ms` is a monotonic millisecond clock (REQUIRED, called by both threads — must be thread-safe), driving CSS/JS timers, layout debouncing, and the VM budget; `sleep_ms` is OPTIONAL (when absent the core uses pthread condition-variable timed waits).

**The sys table**: `ptr_sane` is a **non-dereferencing** in-heap pointer sanity check that the JS bridges run before reading an element's liveness flag (blocking forged handles, see 6.6); `log` outputs core log lines (the text already carries source prefixes like `[js]`).

## 10.6 Reference-Port Comparison: port_ewokos.c vs port_sdl2.c

The repository ships two complete reference ports, both pure C99, both structured as "a set of `ek_*` static functions + `eweb_port_*()` filling the tables":

- **`porting/src/ewokos/port_ewokos.c`** (~400 lines): the native EwokOS graphics stack;
- **`porting/src/sdl2/port_sdl2.c`** (~1400 lines): desktop SDL2, the **default** build's port; it fills more OPTIONAL hooks than the EwokOS port (`surface_pixels`, `flatten_quadratic/cubic`, `text_size`, etc.) and supports HiDPI.

| HAL member | EwokOS implementation | SDL2 implementation |
| --- | --- | --- |
| `eweb_surface_t` | `graph_t*` (plain cast) | `sdl_surf_t{ SDL_Surface(ARGB8888) + SDL_Renderer }` |
| `eweb_font_t` | `font_t*` | `sdl_font_t{ family + TTF_Font* cached by size }` |
| gfx.* | the `graph_*` family, one-to-one (`G(s)`/`SG(g)` macros cast; `surface_native` returns `graph_t*`) | `SDL_FillRect`/`SDL_BlitSurface`/`SDL_BlitScaled` + SDL2_gfx primitives (line/box/circle/arc/pie/rounded/bezier) |
| font.* | `font_new("system-cn")` + `font_metrics`/`font_char_width`/`font_draw_text` | SDL2_ttf (handle carries no size, one `TTF_Font*` cached per size) |
| image.decode | SVG sniff → `graph_image_new_from_data(AUTO)` → WebP falls back to `libwebp` | `IMG_Load_RW`(SDL2_image) + `ConvertSurfaceFormat(ARGB8888)`, WebP via `libwebp` |
| net.request | tinyhttpsc/BearSSL; `SetTimeout(10000)` + **`SetMaxRedirections(0)`** | the same tinyhttpsc/BearSSL; likewise **`SetMaxRedirections(0)`** |
| net.read_file | `vfs_readfile` | standard C `fopen`/`fread` |
| net.resolve_resource | `x_get_res_name` (`res://`) | `<program-dir>/res/<name>` (via `SDL_GetBasePath`) |
| clock | `sys_tic_ms(0)` / `proc_usleep` | `clock_gettime(CLOCK_MONOTONIC)` / `SDL_Delay` |
| sys.log | `klog` | `SDL_Log` |
| sys.ptr_sane | `ewok_ptr_in_heap` | a non-NULL heuristic (desktops have no cheap heap-membership check; OPTIONAL, the core degrades to trusting only the liveness flag) |

**HiDPI**: `eweb_port_sdl2_set_dpr(float)` makes `surface_new` allocate device pixels as `logical size × dpr`, and each draw/font callback scales up the logical coordinates — layout stays in CSS pixels while rasterization runs at native device resolution. The embedder must call it on the UI thread before `ewebview_create()` (sdlbrowser probes it from the `SDL_GetBasePath` side).

**Link on demand**: both port objects compile into `libewebview.a` (`PORTING` decides which one to compile), but the linker pulls one in only if the embedder actually calls `eweb_port_ewokos()` / `eweb_port_sdl2()` — when an external platform links `libewebview.a` and provides its own port, it **introduces no** graph/font/SDL2/tinyhttpsc symbols at all (the Makefile header comment documents this design). Note that `eweb_port_sdl2()` is **not declared in `ewebview_port.h`** (the public header only declares `eweb_port_ewokos()`); an sdl2 embedder must declare it `extern` themselves.

## 10.7 New-Platform Landing Order

Bottom-up by dependency, each step with a verifiable milestone:

1. **`eweb_port_init()` to zero**, fill `clock.tic_ms` — the engine beat, timers, and watchdog budget all depend on it;
2. **the minimal gfx set**: `surface_new/free/dims/clear` + `fill_rect` + `blit` + `blit_fit_alpha`;
3. **the full font set** (the 5 REQUIRED members) — at this point a plain-text page loaded via `ewebview_load("file:...")` can already be laid out on screen;
4. **`net.read_file`** → `file://` local-page loading;
5. **`net.request`/`free_response`** (remember to turn off auto-redirect) → http(s) pages and CSS/image downloads;
6. **`image.decode`** → `<img>` and background images;
7. **fill OPTIONAL as needed**: `surface_pixels` (Canvas ImageData fast path), `surface_set_clip` (rounded corners/clip), `flatten_*` (curve-fill precision), `text_size`, `sleep_ms`, `resolve_resource`, `sys.ptr_sane`;
8. **wire `sys.log` early** — the engine's build-stage timings, char-width cache hit rate, and task-queue activity are all in the log, giving first-hand data for porting-phase debugging.

The degradation behavior when each step is missing is written in the corresponding member's comment in `ewebview_port.h` — the HAL's comments are the contract.

## 10.8 Host Pre-Validation: the sdl2 port + sdlbrowser

When a port has problems, first distinguish a "litehtml integration problem" from a "platform problem". The default `PORTING=sdl2` host build (`make`, see Ch. 2) uses the native compiler to build the engine + `port_sdl2` + `bin/sdlbrowser` into a desktop executable — running real pages **without QEMU / target hardware**. Reproduce layout/script/networking questions first on the host sdlbrowser (it runs the same platform-agnostic core), then decide whether to investigate the porting layer or the core: if the host is fine but the target platform is not, the problem is almost always in one of the target port's tables.

> The old `litehtml/hosttest` diagnostic harness has been removed; its "host-side layout reproduction" duty is now carried by the sdl2 port + sdlbrowser, with more complete coverage (including JS/networking/Canvas, not just layout).

## 10.9 Porting Checklist

- [ ] `tic_ms` monotonic and thread-safe (called by both threads)
- [ ] `surface_free` tolerates NULL; `surface_dims` usable for frame-pool validation
- [ ] `blit_fit_alpha` supports scaling + alpha blending
- [ ] `char_width` is O(1)-level (the layout hot path)
- [ ] `metrics` back-fills the four line-height measures
- [ ] `net.request` **does not auto-follow redirects**; the response lives until `free_response`
- [ ] `image.decode` does not touch engine-thread state (runs on the download thread)
- [ ] handle-cast macros defined centrally (the reference ports' `G/SG/FT/EF` pattern), no scattered casts
- [ ] run test pages end-to-end on real hardware (or the host sdlbrowser): sdlbrowser ships a `res://html/` starter page; the EwokOS repo's `browser/data/test/html/` has fuller DOM/event/form/timer/canvas/storage cases (see Ch. 11 for the embedding side)
