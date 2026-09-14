# 第 2 章 · 目录结构与构建系统

> 语言: [English](02-build.md) | **中文**

## 2.1 仓库布局

ewebview 现在是一个**独立仓库**（不再是某个 `browser/` 树的子目录）。它自带两套参考移植与一个桌面演示壳，既能在宿主机（macOS/Linux）原生构建，也能交叉编译进 EwokOS。

```
ewebview/
├── Makefile            # DIRS = libtinyhttpsc libwebp jsnative litehtml ewebview
│                       #（PORTING=sdl2 时追加 bin/sdlbrowser）
├── make.inc            # 共享构建配置：PORTING / OS_TYPE / ARCH / HW / SDK_DIR
├── host.rule           # 宿主机（原生 gcc/g++，可选 ccache）工具链规则
├── libtinyhttpsc/      # BearSSL HTTP/HTTPS 客户端   -> libtinyhttpsc.a
├── libwebp/            # WebP 解码                    -> libwebp.a
├── litehtml/           # HTML 解析 + CSS 排版         -> liblitehtml.a
│   ├── include/litehtml/   # litehtml 公开头
│   ├── include/gumbo/      # gumbo 公开头（HTML5 解析器，纯 C）
│   └── src/                # litehtml 源（*.cpp）+ src/gumbo/（*.c）
├── mario_js/           # mario 字节码 VM 子模块（内核 + JS 前端 + 内建类）
├── jsnative/           # VM + 浏览器桥               -> libmario_jsn.a
│   └── natives/            # js_dom.c js_event.c js_web.c js_canvas.c
├── ewebview/           # 引擎核心 + 参考移植          -> libewebview.a
│   ├── include/ewebview.h
│   ├── porting/include/ewebview_port.h   # HAL 契约
│   ├── porting/src/sdl2/port_sdl2.c      # 桌面 SDL2 参考移植（默认）
│   ├── porting/src/ewokos/port_ewokos.c  # EwokOS 参考移植
│   └── src/                              # 平台无关核心
├── bin/sdlbrowser/     # SDL2 桌面浏览器壳            -> sdlbrowser
│   └── res/html/           # 内置 default.html / default.css / master.css
└── docs/wiki/          # 本文档
```

## 2.2 构建顺序与依赖方向

顶层 `Makefile` 按固定顺序递归构建：

```
libtinyhttpsc → libwebp → jsnative → litehtml → ewebview  (+ bin/sdlbrowser)
```

`bin/sdlbrowser` 只在 `PORTING=sdl2`（默认）时追加进 `DIRS`。顺序不是随意的：

1. **libtinyhttpsc** 与 **libwebp** 先建，把头文件（`tinyhttpsc/*.h`、`webp.h`）装进 SDK；两份参考移植的 `net.request` 依赖 tinyhttpsc，`image.decode` 依赖 webp。
2. **jsnative** 把 mario VM 与浏览器桥归档为 `libmario_jsn.a`，并把公开头装成 `<mario/*.h>`（`mario.h`、`js_dom.h`、`js_event.h`、`js_web.h`、`js_canvas.h`）；`EWebJs.cc` 依赖这些头。
3. **litehtml** 把 `include/` 整个拷进 SDK 的 `include/`（`<litehtml.h>`、`<litehtml/*.h>`、`<gumbo/*.h>`），ewebview 核心的 `EWebContainer.h` 直接 `#include <litehtml.h>`。
4. **ewebview** 最后构建核心，消费前面所有头文件，并把选中的 port 一起归档。
5. **bin/sdlbrowser** 链接前面全部产物 + SDL2 伴侣库，产出可执行演示。

依赖严格单向：`ewebview 核心 → litehtml / mario 桥 → mario VM`，下层不知道上层存在。移植层（`porting/`）反向依赖平台系统库（SDL2 或 EwokOS graph/font/x），但被隔离在独立编译单元里（见 2.5）。

## 2.3 两种构建模式与配置链

每个子库的 Makefile 头两行都是：

```make
PROJS_ROOT_DIR=..        # bin/sdlbrowser 用 ../..
include $(PROJS_ROOT_DIR)/make.inc
```

`make.inc` 用 `OS_TYPE` 在两种模式间切换：

| 模式 | 触发 | 工具链规则 | SDK_DIR（产物安装处） |
| --- | --- | --- | --- |
| **宿主机原生构建**（默认） | `OS_TYPE` 未设 | `host.rule`（原生 `gcc`/`g++`，装了 `ccache` 就透明套用） | `$(ewebview_root)/build_$(ARCH)/$(HW)`，默认 `build_aarch64/virt/` |
| **EwokOS 交叉构建** | `OS_TYPE=ewokos` | `$(ewokos)/system/platform/$(ARCH)/make.rule`（注入交叉工具链、`-ffreestanding` 等） | `$(ewokos)/system/build_$(ARCH)/$(HW)` |

`make.inc` 还做这些事：

- `ewokos`：EwokOS 源码树根目录，默认由 `make.inc` 自身位置推导为**本仓库的上级目录**（`:=` 立即展开为绝对路径，避免 `~` 被原样交给 gcc），可用 `make ewokos=/path/to/ewokos` 覆盖；
- `ARCH=aarch64`、`HW=virt` 默认值；`ALL_ARCH_DIRS` 列出全部架构输出目录，好让 `clean` 清干净；
- **`-DLITEHTML_LIFETIME_DEBUG` 全局常开**：编译进 litehtml 的节点活性登记与权威活性门（`html_tag` lifetime registry）。关掉它时，被 mario VM 回收复用的内存块可能把"已释放元素"读成"存活"，样式遍历就会拿着悬垂指针崩溃（w3.org 会冻住整个窗口）。**任何构建都不要关它。**

各库产物一律 `ar crs` 成静态库放进 `$(SDK_DIR)/lib`，公开头拷进 `$(SDK_DIR)/include`——EwokOS 用户态只有静态链接，宿主机演示也走同一套静态产物。

## 2.4 五个子库的产物

### libtinyhttpsc（`libtinyhttpsc/Makefile`）

自包含的 BearSSL HTTP/HTTPS 客户端 amalgamation，归档 `libtinyhttpsc.a`，头文件 `tinyhttpsc/*.h` 拷入 SDK。它只需要 SDK 安装的 libc/socket 头（`sys/socket.h` 等），BearSSL 引擎内嵌在 `src/BearHttpsClientOne.c`，**没有外部 TLS 依赖**。两份参考移植的 `net.request` 都建在它之上。

### libwebp（`libwebp/Makefile`）

裁剪版 WebP 解码，归档 `libwebp.a`，头文件 `webp.h` 拷入 SDK。暴露 `webp_is_webp()` / `webp_decode()` / `webp_image_free()`，像素格式 ARGB8888。移植层的 `image.decode` 用它补平台图像库没有的 WebP。

### litehtml（`litehtml/Makefile`）

- `LITEHTML_OBJS`：litehtml 排版引擎全部 `.cpp`（元素类 `el_*.o`、`document.o`、`stylesheet.o`、`css_selector.o`、`media_query.o`、`flex_layout.o` 等）；
- `GUMBO_OBJS`：gumbo 解析器全部 `.c`（tokenizer、parser、utf8、char_ref……）；
- 两者合入同一个 `liblitehtml.a`；
- flags：`-fno-rtti`、`-MMD -MP`（**头文件依赖追踪必须保留**——`element.h`/`html_tag.h` 这类公共头改动时，没有 `.d` 会让旧对象混进新归档，vtable 错位导致难查的运行时崩溃）；
- **`-nostdinc++` 只对 `PORTING=ewokos` 施加**：EwokOS 用 SDK 里的 EWOK_STL 作为唯一 C++ 标准库，需要隐藏工具链自带的 C++ 头；而原生 sdl2 构建 against 宿主机 libc++/libstdc++，其 SDK include 目录不带 STL，加上 `-nostdinc++` 会让 `<string>`/`<vector>` 找不到。

### jsnative（`jsnative/Makefile`）

目录里没有 VM 本体——`MARIO_VM = ../mario_js`，它把子模块里的 VM 源与本地桥一起归档：

```
libmario_jsn.a =
  VM 内核      mario/mario.c mario/lex/mario_lex.c mario/bcdump/bcdump.c
  JS 编译器    lang/js/compiler.c
  内建类       native/builtin/{Object,Array,String,Console,Promise,Map,Set,
               Symbol,Proxy,Reflect,WeakRef,FinalizationRegistry,
               SharedArrayBuffer,Atomics,RegExp,BigInt,ArrayBuffer,DataView,
               TypedArray,Number,Error}
               native/natives/{JSON,Date,Math}
  浏览器桥     jsnative/natives/{js_dom,js_event,js_web,js_canvas}.c
```

头文件以 `<mario/*.h>` 命名空间安装（`js_natives_priv.h` 保持私有）。**库名 `libmario_jsn.a`（jsn = JS natives）**，目录名 `jsnative` 只是源码组织（早前叫 `mario/`），头文件命名空间保持 `mario` 不变以兼容存量代码。

### ewebview（`ewebview/Makefile`）

```make
EWEBVIEW_OBJS = ewebview.o EWebContainer.o EWebCanvas.o EWebCanvasGlue.o \
                EWebJs.o EWebCookies.o eweb_el_input.o     # C++14，平台无关
PORT_OBJS     = $(ARCH)/porting/$(PORTING)/port_$(PORTING).o   # C99，选中的参考移植
TASK = $(TARGET_DIR)/lib/libewebview.a
```

关键设计：

- **`PORTING` 决定哪份 port 进归档**：`PORTING=sdl2`（默认）编 `port_sdl2.o`，`PORTING=ewokos` 编 `port_ewokos.o`。`PORTING=sdl2` 时 Makefile 还会用 `sdl2-config --cflags` 把 SDL2 及伴侣库（SDL2_image/_ttf/_gfx）的头路径与 `-D_THREAD_SAFE` 加进来，跨 Apple Silicon `/opt/homebrew`、Intel `/usr/local` 与 Linux 发行版都可移植；这段只在 `PORTING=sdl2` 且 `sdl2-config` 存在时生效。
- **核心与参考移植打进同一个归档，但移植层是按需链接的**：port 对象只在嵌入者引用 `eweb_port_sdl2()` / `eweb_port_ewokos()` 时才被 `ld` 从归档里拉出来——所以一个外来平台可以链接 `libewebview.a` 并自带 port，而不会把 SDL2 或 graph/font/tinyhttpsc 的符号依赖拖进来。这是"核心可移植"在构建系统层面的落地。

安装的头文件只有两个：`ewebview.h`（公开 API）与 `ewebview_port.h`（HAL 契约）。注意 `eweb_port_sdl2()` **未在公开头里声明**（`ewebview_port.h` 只声明了 `eweb_port_ewokos()`），sdl2 嵌入者需自行 `extern` 声明它（见 `bin/sdlbrowser/main.c`）。

## 2.5 应用侧链接

以宿主机演示 `bin/sdlbrowser/Makefile` 为例：

```make
LIBS = -lewebview -llitehtml -lmario_jsn -lwebp -ltinyhttpsc \
       $(SDL2_LIBS) -lm -lpthread
# macOS 外观探测（AppleInterfaceStyle）用到 CoreFoundation
ifeq ($(shell uname -s),Darwin)
LIBS += -framework CoreFoundation
endif
```

要点：

- 静态归档按**依赖序**排列：调用者在前，被调者在后（`-lewebview` 先于 `-llitehtml`/`-lmario_jsn`，`-ltinyhttpsc` 兜底网络符号）；
- `libewebview.a` 内部是 C++14，用 `$(CXX)` 链接以带进 libstdc++；
- SDL2 及伴侣库的 include/link flags 由 `pkg-config`（`sdl2 SDL2_ttf SDL2_gfx`，`SDL2_image` 无 `.pc` 时退回 `-lSDL2_image`）或 `sdl2-config` 解析；
- `PROJ_LIBS` 把五个 `.a` 列为链接前提，于是"只改了引擎、`main.o` 没变"时也会重链 `sdlbrowser`，不会留下陈旧二进制。

EwokOS 侧的 xBrowser 链接形如：

```
$(LD) -Ttext=100 main.o -o xBrowser \
    $(EWOK_LIB_X) -lWidgetWebview -lewebview -lmario_jsn -lwebp -llitehtml \
    -ltinyhttpsc -lsocket $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
```

`-Ttext=100` 是 EwokOS 用户二进制统一的静态非 PIE 布局；`port_ewokos.o` 把 `graph/font/tinyhttpsc/socket/vfs/x` 符号引入，因此后面必须跟 `$(EWOK_LIB_GRAPH)`、`-ltinyhttpsc -lsocket` 等系统库——这正是 2.4 说的"按需拉入"。

## 2.6 运行 sdlbrowser

宿主机构建产出 `build_aarch64/virt/bin/sdlbrowser`，并把 `bin/sdlbrowser/res/` 拷成同级的 `res/`（`res://` 协议根，见第 9/10 章）：

```sh
make                                        # 默认 PORTING=sdl2，宿主机原生构建
./build_aarch64/virt/bin/sdlbrowser          # 载入 res://html/default.html
./build_aarch64/virt/bin/sdlbrowser https://www.w3.org
```

运行期需要 SDL2 / SDL2_ttf / SDL2_image / SDL2_gfx（macOS：`brew install sdl2 sdl2_ttf sdl2_image sdl2_gfx`；Linux：发行版对应包）。`sdlbrowser` 自带地址栏、状态栏、前进/后退/停止/刷新、滚轮滚动与 HiDPI 缩放，是引擎 + `port_sdl2` 的最小完整嵌入者（第 11 章逐一对照）。

## 2.7 调试开关

| 开关 | 位置 | 作用 |
| --- | --- | --- |
| `-DLITEHTML_LIFETIME_DEBUG` | `make.inc` / `host.rule`（常开） | litehtml 节点活性登记，防悬垂指针（见 2.3） |
| `-DEWEBVIEW_DEBUG` | 编译 ewebview 时加 | 打开核心 `EWEB_LOG`（`EWebLog.h`，默认编译为空操作，输出走 `sys.log`/stderr） |
| `DEBUG=yes` | `host.rule` | 宿主机构建带 `-g` 而非 `-O2` |
| `sys.log` 钩子 | 移植表 | JS `console.*` 与 `[ewebview]` 引擎日志的出口（EwokOS 上是 `klog`，SDL2 上是 `SDL_Log`） |

## 2.8 常见构建陷阱

1. **改了公共头但对象没重编** → 确认 `-MMD -MP` 与 `-include $(OBJS:.o=.d)` 存在（litehtml/ewebview/jsnative 的 Makefile 都有）。
2. **新增子目录忘记登记** → 顶层 `Makefile` 的 `DIRS` 是显式列表，不是通配。
3. **宿主机构建报找不到 `<string>`/`<vector>`** → 检查是否误把 `-nostdinc++` 施加到了非 ewokos 构建（该 flag 只应在 `PORTING=ewokos` 时出现，见 2.4）。
4. **链接报缺 graph/font 符号** → 检查是否引用了 `eweb_port_ewokos()` 但漏了 `$(EWOK_LIB_GRAPH)`；反之纯外来 port 或 sdl2 port 不应出现这些符号。
5. **sdl2 构建报缺 SDL 头/库** → 确认装了 `sdl2-config` 或 `pkg-config` 能解析 `sdl2 SDL2_ttf SDL2_gfx`（`SDL2_image` 无 `.pc` 时靠 `-lSDL2_image` 兜底）。
6. **`mario_js` 子模块未初始化** → `git submodule update --init`（jsnative 编译 `../mario_js/...` 的源，为空目录时报找不到文件）。
