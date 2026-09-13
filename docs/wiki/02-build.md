# 第 2 章 · 目录结构与构建系统

## 2.1 仓库布局

ewebview 位于 `browser/ewebview/`，是 `browser/` 仓库的一个子树。`browser/Makefile` 的构建顺序是 `DIRS = ewebview libs apps`：先构建本引擎，再构建 widget++ 等库，最后是 xBrowser 等应用。

```
browser/ewebview/
├── Makefile            # DIRS = libwebp litehtml jsnative ewebview
├── libwebp/            # WebP 解码            -> libwebp.a
├── litehtml/           # HTML 解析 + CSS 排版  -> liblitehtml.a
│   ├── include/litehtml/   # litehtml 公开头
│   ├── include/gumbo/      # gumbo 公开头（HTML5 解析器，纯 C）
│   ├── include/master.css  # UA 默认样式表
│   ├── src/                # litehtml 源（*.cpp）+ src/gumbo/（*.c）
│   └── hosttest/           # 宿主机诊断 harness（不参与交叉编译）
├── mario_js/           # mario 字节码 VM 子模块（内核 + JS 前端 + 内建类）
├── jsnative/           # 浏览器桥 natives     -> 与 VM 合并为 libmario.a
│   └── natives/            # js_dom.c js_event.c js_web.c js_canvas.c
└── ewebview/           # 引擎核心             -> libewebview.a
    ├── include/ewebview.h
    ├── porting/            # HAL 契约 + EwokOS 参考实现
    └── src/                # 平台无关核心
```

## 2.2 构建顺序与依赖方向

顶层 `Makefile` 按固定顺序递归构建：

```
libwebp → litehtml → jsnative → ewebview
```

顺序不是随意的：

1. **litehtml** 把 `include/` 整个拷进 SDK 的 `include/`（`<litehtml.h>`、`<litehtml/*.h>`、`<gumbo/*.h>`），ewebview 核心的 `EWebContainer.h` 直接 `#include <litehtml.h>`。
2. **jsnative** 把 mario VM 与浏览器桥归档为 `libmario.a`，并把公开头装成 `<mario/*.h>`（`mario.h`、`js_dom.h`、`js_event.h`、`js_web.h`、`js_canvas.h`）；`EWebJs.cc` 依赖这些头。
3. **ewebview** 最后构建，消费前两者的头文件。

依赖严格单向：`ewebview 核心 → litehtml / mario 桥 → mario VM`，下层不知道上层存在。移植层（`porting/`）反向依赖 EwokOS 系统库，但被隔离在独立编译单元里（见 2.5）。

## 2.3 交叉编译配置链

每个子库的 Makefile 头两行都是：

```make
PROJS_ROOT_DIR=../..
include $(PROJS_ROOT_DIR)/$(PORTING).inc
```

`browser/$(PORTING).inc` 做的事：

- `ewokos`：EwokOS 源码树根目录，默认取本仓库根（`$(PORTING).inc` 自身位置推导），可用 `make ewokos=/path/to/ewokos` 覆盖；
- `ARCH=aarch64`、`HW=virt` 默认值；
- `SDK_DIR = $(ewokos)/system/build_$(ARCH)/$(HW)`——所有库的 `lib*.a` 与头文件都安装到这里；
- `include $(EWOKOS_SYS_DIR)/platform/$(ARCH)/make.rule`——注入交叉工具链（`CROSS_COMPILE`）、`-ffreestanding` 等通用 flags；
- **`-DLITEHTML_LIFETIME_DEBUG` 全局常开**：编译进 litehtml 的节点活性登记与权威活性门（`html_tag` lifetime registry）。关掉它时，被 mario VM 回收复用的内存块可能把"已释放元素"读成"存活"，样式遍历就会拿着悬垂指针崩溃。**任何构建都不要关它。**

各库 `BUILD_DIR = $(EWOKOS_SYS_DIR)/build_$(ARCH)/$(HW)`，产物一律 `ar crs` 成静态库放进 `$(SDK_DIR)/lib`——EwokOS 用户态只有静态链接。

## 2.4 四个子库的产物

### libwebp（`libwebp/Makefile`）

裁剪版 WebP 解码，归档 `libwebp.a`，头文件 `webp.h` 拷入 SDK。暴露 `webp_is_webp()` / `webp_decode()` / `webp_image_free()`，像素格式 ARGB8888。EwokOS 移植层的 `image.decode` 用它补 graph_image 没有的 WebP。

### litehtml（`litehtml/Makefile`）

- `LITEHTML_OBJS`：litehtml 排版引擎全部 `.cpp`（元素类 `el_*.o`、`document.o`、`stylesheet.o`、`css_selector.o`、`media_query.o`、`flex_layout.o` 等）；
- `GUMBO_OBJS`：gumbo 解析器全部 `.c`（tokenizer、parser、utf8、char_ref……）；
- 两者合入同一个 `liblitehtml.a`；
- flags：`-fno-rtti -nostdinc++`（freestanding 兼容）、`-MMD -MP`（**头文件依赖追踪必须保留**——`element.h`/`html_tag.h` 这类公共头改动时，没有 `.d` 会让旧对象混进新归档，vtable 错位导致难查的运行时崩溃）。

### jsnative（`jsnative/Makefile`）

目录里没有 VM 本体——`MARIO_VM = ../mario_js`，它把子模块里的 VM 源与本地桥一起归档：

```
libmario.a =
  VM 内核      mario/mario.c mario/lex/mario_lex.c mario/bcdump/bcdump.c
  JS 编译器    lang/js/compiler.c
  内建类       native/builtin/{Object,Array,String,Console,Promise,Map,Set,...}
               native/natives/{JSON,Date,Math}
  浏览器桥     jsnative/natives/{js_dom,js_event,js_web,js_canvas}.c
```

头文件以 `<mario/*.h>` 命名空间安装。**目录名 `jsnative` 只是源码组织**（早前叫 `mario/`），库名与头文件命名空间保持 `mario` 不变以兼容存量代码。

### ewebview（`ewebview/Makefile`）

```make
EWEBVIEW_OBJS = ewebview.o EWebContainer.o EWebCanvas.o EWebCanvasGlue.o \
                EWebJs.o EWebCookies.o eweb_el_input.o     # C++14，平台无关
PORT_OBJS     = port_ewokos.o                              # C99，EwokOS 参考移植
TASK = $(TARGET_DIR)/lib/libewebview.a
```

关键设计：**核心与参考移植打进同一个归档，但移植层是按需链接的**。`port_ewokos.o` 只在嵌入者引用 `eweb_port_ewokos()` 时才被 `ld` 从归档里拉出来——所以一个外来平台可以链接 `libewebview.a` 并自带 port，而不会把 graph/font/tinyhttpsc 的符号依赖拖进来。这是"核心可移植"在构建系统层面的落地。

安装的头文件只有两个：`ewebview.h`（公开 API）与 `ewebview_port.h`（HAL 契约）。

## 2.5 应用侧链接

以 xBrowser（`browser/apps/xBrowser/Makefile`）为例：

```
$(LD) -Ttext=100 main.o -o xBrowser \
    $(EWOK_LIB_X) -lWidgetWebview -lewebview -lmario -lwebp -llitehtml \
    -ltinyhttpsc -lsocket $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
```

要点：

- 静态归档按**依赖序**排列：调用者在前，被调者在后（`-lewebview` 先于 `-llitehtml`，`-lmario` 在 `-lewebview` 之后等）；
- `-Ttext=100`：EwokOS 用户二进制统一静态非 PIE 布局；
- `libewebview.a` 里的 `port_ewokos.o` 把 `graph/font/tinyhttpsc/socket/vfs/x` 等符号引入，因此后面必须跟 `$(EWOK_LIB_GRAPH)`、`-ltinyhttpsc -lsocket` 等系统库——这正是 2.4 说的"按需拉入"。

## 2.6 宿主机诊断 harness（hosttest）

`litehtml/hosttest/` 不参与交叉编译，是一个**在 macOS 上用 clang 直接构建 litehtml + gumbo** 的诊断程序：

```sh
cd litehtml/hosttest && ./build.sh   # 产出 build/lhtest
```

- `main.cpp`：一个极简 `document_container`（假字体度量、记录 draw 调用），加载真实 HTML+CSS 后 dump 计算样式与几何；
- `stub/ewoksys/*.h`、`stubs.c`：把 litehtml 里引用的少量 EwokOS 符号（`sys_tic_ms`、`klog`）打桩；
- 用途：**不起 QEMU 就能复现排版问题**（样式缺失、宽高异常、选择器匹配）。移植或修 litehtml bug 时先在这里验证，比全链路 QEMU 快几个数量级。

## 2.7 调试开关

| 开关 | 位置 | 作用 |
| --- | --- | --- |
| `-DLITEHTML_LIFETIME_DEBUG` | `browser/make.inc`（常开） | litehtml 节点活性登记，防悬垂指针（见 2.3） |
| `-DEWEBVIEW_DEBUG` | 编译 ewebview 时加 | 打开核心 `EWEB_LOG`（默认编译为空操作，输出走 stderr） |
| `XBROWSER_HEAPSTAT=1` | 运行时环境变量 | WidgetWebview 定时打印堆统计 |
| `sys.log` 钩子 | 移植表 | JS `console.*` 与 `[ewebview]` 引擎日志的出口（EwokOS 上是 `klog`） |

## 2.8 常见构建陷阱

1. **改了公共头但对象没重编** → 确认 `-MMD -MP` 与 `-include $(OBJS:.o=.d)` 存在（litehtml/ewebview 的 Makefile 都有）。
2. **新增子目录忘记登记** → 顶层 `Makefile` 的 `DIRS` 是显式列表，不是通配。
3. **链接报缺 graph/font 符号** → 检查是否引用了 `eweb_port_ewokos()` 但漏了 `$(EWOK_LIB_GRAPH)`；反之纯外来 port 不应出现这些符号。
4. **`mario_js` 子模块未初始化** → `git submodule update --init`（jsnative 编译 `../mario_js/...` 的源，为空目录时报找不到文件）。
