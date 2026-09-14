# Chapter 2 · Directory Layout & Build System

> Language: **English** | [中文](02-build.zh.md)

## 2.1 Repository Layout

ewebview is now a **standalone repository** (no longer a subdirectory of some `browser/` tree). It carries two reference ports and a desktop demo shell, and builds both natively on the host (macOS/Linux) and cross-compiled into EwokOS.

```
ewebview/
├── Makefile            # DIRS = libtinyhttpsc libwebp jsnative litehtml ewebview
│                       # (+ bin/sdlbrowser when PORTING=sdl2)
├── make.inc            # shared build config: PORTING / OS_TYPE / ARCH / HW / SDK_DIR
├── host.rule           # host (native gcc/g++, optional ccache) toolchain rules
├── libtinyhttpsc/      # BearSSL HTTP/HTTPS client    -> libtinyhttpsc.a
├── libwebp/            # WebP decoding                -> libwebp.a
├── litehtml/           # HTML parsing + CSS layout     -> liblitehtml.a
│   ├── include/litehtml/   # litehtml public headers
│   ├── include/gumbo/      # gumbo public headers (HTML5 parser, pure C)
│   └── src/                # litehtml sources (*.cpp) + src/gumbo/ (*.c)
├── mario_js/           # mario bytecode VM submodule (kernel + JS frontend + builtin classes)
├── jsnative/           # VM + browser bridges         -> libmario_jsn.a
│   └── natives/            # js_dom.c js_event.c js_web.c js_canvas.c
├── ewebview/           # engine core + reference ports -> libewebview.a
│   ├── include/ewebview.h
│   ├── porting/include/ewebview_port.h   # HAL contract
│   ├── porting/src/sdl2/port_sdl2.c      # desktop SDL2 reference port (default)
│   ├── porting/src/ewokos/port_ewokos.c  # EwokOS reference port
│   └── src/                              # platform-agnostic core
├── bin/sdlbrowser/     # SDL2 desktop browser shell   -> sdlbrowser
│   └── res/html/           # bundled default.html / default.css / master.css
└── docs/wiki/          # this documentation
```

## 2.2 Build Order & Dependency Direction

The top-level `Makefile` builds recursively in a fixed order:

```
libtinyhttpsc → libwebp → jsnative → litehtml → ewebview  (+ bin/sdlbrowser)
```

`bin/sdlbrowser` is appended to `DIRS` only when `PORTING=sdl2` (the default). The order is not arbitrary:

1. **libtinyhttpsc** and **libwebp** build first, installing their headers (`tinyhttpsc/*.h`, `webp.h`) into the SDK; both reference ports' `net.request` depends on tinyhttpsc and `image.decode` depends on webp.
2. **jsnative** archives the mario VM and browser bridges into `libmario_jsn.a` and installs its public headers as `<mario/*.h>` (`mario.h`, `js_dom.h`, `js_event.h`, `js_web.h`, `js_canvas.h`); `EWebJs.cc` depends on these headers.
3. **litehtml** copies its entire `include/` into the SDK's `include/` (`<litehtml.h>`, `<litehtml/*.h>`, `<gumbo/*.h>`); the ewebview core's `EWebContainer.h` directly `#include <litehtml.h>`.
4. **ewebview** builds the core last, consuming all preceding headers and archiving the selected port alongside it.
5. **bin/sdlbrowser** links all preceding artifacts + the SDL2 companion libraries into an executable demo.

Dependencies are strictly one-directional: `ewebview core → litehtml / mario bridges → mario VM`; lower layers do not know upper layers exist. The porting layer (`porting/`) depends in the reverse direction on platform system libraries (SDL2 or EwokOS graph/font/x), but is isolated in its own compilation unit (see 2.5).

## 2.3 Two Build Modes & the Config Chain

The first two lines of every sub-library's Makefile are:

```make
PROJS_ROOT_DIR=..        # bin/sdlbrowser uses ../..
include $(PROJS_ROOT_DIR)/make.inc
```

`make.inc` switches between two modes via `OS_TYPE`:

| Mode | Trigger | Toolchain rules | SDK_DIR (install target) |
| --- | --- | --- | --- |
| **Host native build** (default) | `OS_TYPE` unset | `host.rule` (native `gcc`/`g++`, transparently wrapped with `ccache` if installed) | `$(ewebview_root)/build_$(ARCH)/$(HW)`, default `build_aarch64/virt/` |
| **EwokOS cross build** | `OS_TYPE=ewokos` | `$(ewokos)/system/platform/$(ARCH)/make.rule` (injects the cross toolchain, `-ffreestanding`, etc.) | `$(ewokos)/system/build_$(ARCH)/$(HW)` |

`make.inc` also does the following:

- `ewokos`: the EwokOS source-tree root, defaulting to **the parent directory of this repository**, derived from `make.inc`'s own location (`:=` expands immediately to an absolute path so `~` is never handed literally to gcc); overridable with `make ewokos=/path/to/ewokos`;
- `ARCH=aarch64`, `HW=virt` defaults; `ALL_ARCH_DIRS` lists all architecture output directories so `clean` cleans them thoroughly;
- **`-DLITEHTML_LIFETIME_DEBUG` is globally always-on**: it compiles in litehtml's node-liveness registration and the authoritative liveness gate (the `html_tag` lifetime registry). When turned off, memory blocks recycled by the mario VM may read a "freed element" as "alive", and style traversal will crash on a dangling pointer (w3.org freezes the entire window). **Never turn it off in any build.**

Each library's artifacts are always `ar crs`-archived into a static library placed in `$(SDK_DIR)/lib`, with public headers copied into `$(SDK_DIR)/include` — EwokOS userland only has static linking, and the host demo uses the same static artifacts.

## 2.4 Artifacts of the Five Sub-Libraries

### libtinyhttpsc (`libtinyhttpsc/Makefile`)

A self-contained BearSSL HTTP/HTTPS client amalgamation, archived as `libtinyhttpsc.a`, with headers `tinyhttpsc/*.h` copied into the SDK. It needs only the libc/socket headers installed by the SDK (`sys/socket.h`, etc.); the BearSSL engine is embedded in `src/BearHttpsClientOne.c`, with **no external TLS dependency**. Both reference ports' `net.request` are built on it.

### libwebp (`libwebp/Makefile`)

A trimmed WebP decoder, archived as `libwebp.a`, with the header `webp.h` copied into the SDK. It exposes `webp_is_webp()` / `webp_decode()` / `webp_image_free()`, pixel format ARGB8888. The porting layer's `image.decode` uses it to supply the WebP support the platform image library lacks.

### litehtml (`litehtml/Makefile`)

- `LITEHTML_OBJS`: all `.cpp` of the litehtml layout engine (element classes `el_*.o`, `document.o`, `stylesheet.o`, `css_selector.o`, `media_query.o`, `flex_layout.o`, etc.);
- `GUMBO_OBJS`: all `.c` of the gumbo parser (tokenizer, parser, utf8, char_ref…);
- both merged into the same `liblitehtml.a`;
- flags: `-fno-rtti`, `-MMD -MP` (**header dependency tracking must be kept** — when a public header like `element.h`/`html_tag.h` changes, missing `.d` files let stale objects mix into the new archive, causing vtable misalignment and hard-to-diagnose runtime crashes);
- **`-nostdinc++` is applied only for `PORTING=ewokos`**: EwokOS uses the SDK's EWOK_STL as its sole C++ standard library and needs the toolchain's own C++ headers hidden; whereas the native sdl2 build compiles against the host libc++/libstdc++, whose SDK include directory carries no STL, so adding `-nostdinc++` would make `<string>`/`<vector>` unfindable.

### jsnative (`jsnative/Makefile`)

The directory contains no VM body itself — `MARIO_VM = ../mario_js`; it archives the submodule's VM sources together with the local bridges:

```
libmario_jsn.a =
  VM kernel      mario/mario.c mario/lex/mario_lex.c mario/bcdump/bcdump.c
  JS compiler    lang/js/compiler.c
  builtin classes native/builtin/{Object,Array,String,Console,Promise,Map,Set,
               Symbol,Proxy,Reflect,WeakRef,FinalizationRegistry,
               SharedArrayBuffer,Atomics,RegExp,BigInt,ArrayBuffer,DataView,
               TypedArray,Number,Error}
               native/natives/{JSON,Date,Math}
  browser bridges jsnative/natives/{js_dom,js_event,js_web,js_canvas}.c
```

Headers install under the `<mario/*.h>` namespace (`js_natives_priv.h` stays private). **The library name is `libmario_jsn.a` (jsn = JS natives)**; the directory name `jsnative` is only source organization (it was earlier called `mario/`), and the header namespace stays `mario` for compatibility with existing code.

### ewebview (`ewebview/Makefile`)

```make
EWEBVIEW_OBJS = ewebview.o EWebContainer.o EWebCanvas.o EWebCanvasGlue.o \
                EWebJs.o EWebCookies.o eweb_el_input.o     # C++14, platform-agnostic
PORT_OBJS     = $(ARCH)/porting/$(PORTING)/port_$(PORTING).o   # C99, the selected reference port
TASK = $(TARGET_DIR)/lib/libewebview.a
```

Key design:

- **`PORTING` decides which port goes into the archive**: `PORTING=sdl2` (default) compiles `port_sdl2.o`; `PORTING=ewokos` compiles `port_ewokos.o`. When `PORTING=sdl2`, the Makefile also uses `sdl2-config --cflags` to add the header paths of SDL2 and its companion libraries (SDL2_image/_ttf/_gfx) plus `-D_THREAD_SAFE`, portable across Apple Silicon `/opt/homebrew`, Intel `/usr/local`, and Linux distributions; this block takes effect only when `PORTING=sdl2` and `sdl2-config` exists.
- **The core and reference port are packed into the same archive, but the porting layer is linked on demand**: a port object is pulled out of the archive by `ld` only when the embedder references `eweb_port_sdl2()` / `eweb_port_ewokos()` — so a foreign platform can link `libewebview.a` and bring its own port without dragging in SDL2 or graph/font/tinyhttpsc symbol dependencies. This is how "portable core" lands at the build-system level.

Only two headers are installed: `ewebview.h` (public API) and `ewebview_port.h` (HAL contract). Note that `eweb_port_sdl2()` is **not declared in a public header** (`ewebview_port.h` only declares `eweb_port_ewokos()`); an sdl2 embedder must declare it `extern` themselves (see `bin/sdlbrowser/main.c`).

## 2.5 Application-Side Linking

Take the host demo `bin/sdlbrowser/Makefile` as an example:

```make
LIBS = -lewebview -llitehtml -lmario_jsn -lwebp -ltinyhttpsc \
       $(SDL2_LIBS) -lm -lpthread
# macOS appearance detection (AppleInterfaceStyle) uses CoreFoundation
ifeq ($(shell uname -s),Darwin)
LIBS += -framework CoreFoundation
endif
```

Key points:

- Static archives are ordered by **dependency order**: caller first, callee after (`-lewebview` before `-llitehtml`/`-lmario_jsn`, `-ltinyhttpsc` backstopping network symbols);
- `libewebview.a` is C++14 internally, linked with `$(CXX)` to bring in libstdc++;
- SDL2 and companion-library include/link flags are resolved by `pkg-config` (`sdl2 SDL2_ttf SDL2_gfx`, falling back to `-lSDL2_image` when `SDL2_image` has no `.pc`) or by `sdl2-config`;
- `PROJ_LIBS` lists the five `.a` files as link prerequisites, so even when "only the engine changed and `main.o` is unchanged" `sdlbrowser` is re-linked, leaving no stale binary.

On the EwokOS side, xBrowser links like this:

```
$(LD) -Ttext=100 main.o -o xBrowser \
    $(EWOK_LIB_X) -lWidgetWebview -lewebview -lmario_jsn -lwebp -llitehtml \
    -ltinyhttpsc -lsocket $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
```

`-Ttext=100` is the uniform static non-PIE layout for EwokOS user binaries; `port_ewokos.o` pulls in `graph/font/tinyhttpsc/socket/vfs/x` symbols, so the system libraries `$(EWOK_LIB_GRAPH)`, `-ltinyhttpsc -lsocket`, etc. must follow — this is exactly the "pull in on demand" described in 2.4.

## 2.6 Running sdlbrowser

The host build produces `build_aarch64/virt/bin/sdlbrowser` and copies `bin/sdlbrowser/res/` into a sibling `res/` (the `res://` protocol root, see Ch. 9/10):

```sh
make                                        # default PORTING=sdl2, host native build
./build_aarch64/virt/bin/sdlbrowser          # loads res://html/default.html
./build_aarch64/virt/bin/sdlbrowser https://www.w3.org
```

At runtime it needs SDL2 / SDL2_ttf / SDL2_image / SDL2_gfx (macOS: `brew install sdl2 sdl2_ttf sdl2_image sdl2_gfx`; Linux: the corresponding distro packages). `sdlbrowser` comes with an address bar, status bar, forward/back/stop/reload, scroll-wheel scrolling, and HiDPI scaling — the minimal complete embedder of engine + `port_sdl2` (Ch. 11 walks through it point by point).

## 2.7 Debug Switches

| Switch | Location | Effect |
| --- | --- | --- |
| `-DLITEHTML_LIFETIME_DEBUG` | `make.inc` / `host.rule` (always on) | litehtml node-liveness registration, preventing dangling pointers (see 2.3) |
| `-DEWEBVIEW_DEBUG` | add when compiling ewebview | enables the core `EWEB_LOG` (`EWebLog.h`, compiled to a no-op by default; output goes through `sys.log`/stderr) |
| `DEBUG=yes` | `host.rule` | host build carries `-g` instead of `-O2` |
| `sys.log` hook | porting table | the outlet for JS `console.*` and `[ewebview]` engine logs (`klog` on EwokOS, `SDL_Log` on SDL2) |

## 2.8 Common Build Pitfalls

1. **Changed a public header but objects didn't recompile** → confirm `-MMD -MP` and `-include $(OBJS:.o=.d)` exist (the litehtml/ewebview/jsnative Makefiles all have them).
2. **Forgot to register a new subdirectory** → the top-level `Makefile`'s `DIRS` is an explicit list, not a wildcard.
3. **Host build reports `<string>`/`<vector>` not found** → check whether `-nostdinc++` was mistakenly applied to a non-ewokos build (that flag should only appear when `PORTING=ewokos`, see 2.4).
4. **Link reports missing graph/font symbols** → check whether `eweb_port_ewokos()` was referenced but `$(EWOK_LIB_GRAPH)` omitted; conversely a purely foreign port or the sdl2 port should not surface these symbols.
5. **sdl2 build reports missing SDL headers/libraries** → confirm `sdl2-config` is installed or `pkg-config` can resolve `sdl2 SDL2_ttf SDL2_gfx` (`SDL2_image` falls back to `-lSDL2_image` when it has no `.pc`).
6. **`mario_js` submodule not initialized** → `git submodule update --init` (jsnative compiles sources from `../mario_js/...`; an empty directory reports files not found).
