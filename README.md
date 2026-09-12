# ewebview

**ewebview** 是一个自包含的嵌入式 Web 引擎（HTML + CSS + JavaScript），面向资源受限的操作系统与裸机环境。它把网页渲染成一块抽象的 ARGB8888 内存画布（`eweb_surface_t`），平台相关的图形、字体、图片解码、网络与时钟全部通过 `eweb_port_t` 回调表（HAL）注入，核心代码不依赖任何具体操作系统、窗口系统或图形库。

本仓库的参考平台是 EwokOS（`ewebview/porting/src/port_ewokos.c`），浏览器应用 xBrowser 即构建在它之上。

## 特性

- **HTML5 / CSS**：litehtml 排版引擎 + Google Gumbo 解析器（vendored，见 `litehtml/`），支持常用 CSS2.1/3 属性、媒体查询、表格与 flex 布局。
- **JavaScript**：mario 字节码虚拟机（`mario_js/` 子模块），支持 ES5 主体 + 大量 ES6+ 特性；DOM / Event / BOM / Canvas 2D 四套纯 C 桥接层（`jsnative/`）。
- **Canvas 2D**：完整的 `CanvasRenderingContext2D`（路径、渐变、图案、虚线、阴影、ImageData、drawImage、Path2D）。
- **网络**：http/https（BearSSL）、`file://`、私有 `res://` 资源协议；重定向由核心逐跳跟随并按跳重配 Cookie。
- **多线程流水线**：引擎线程 + 按需下载线程，UI 线程零阻塞；帧池所有权转移 + 背压，跨线程零像素拷贝。
- **可移植**：全部平台依赖收敛为 6 张回调表（gfx / font / image / net / clock / sys），其中只有 gfx + font + clock 是必需的，其余可增量补齐。

## 目录结构

```
ewebview/
├── Makefile          # 顶层构建：DIRS = libwebp litehtml jsnative ewebview
├── ewebview/         # 引擎核心（libewebview.a）
│   ├── include/ewebview.h           # 公开 C API
│   ├── src/                         # 平台无关的 C++14 核心
│   │   ├── ewebview.cc              #   C API、引擎线程、build 状态机、帧池
│   │   ├── EWebContainer.{h,cc}     #   litehtml document_container 实现
│   │   ├── EWebJs.cc                #   mario VM 生命周期 + DOM/Event/Web 桥回调
│   │   ├── EWebCanvas.{h,cc}        #   <canvas> 离屏画布与绘图原语
│   │   ├── EWebCanvasGlue.cc        #   Canvas 桥 trampoline + 画布注册表
│   │   ├── EWebCookies.{h,cc}       #   进程级 CookieJar（SameSite 隔离）
│   │   └── eweb_el_input.{h,cc}     #   <input> 替换元素
│   └── porting/                     # 平台移植层（HAL）
│       ├── include/ewebview_port.h  #   eweb_port_t 六张回调表的契约
│       └── src/port_ewokos.c        #   EwokOS 参考实现（graph/font/tinyhttpsc）
├── jsnative/         # JS 原生桥（打进 libmario.a）：js_dom/js_event/js_web/js_canvas
├── mario_js/         # mario 字节码 VM 子模块（内核 + JS 前端 + 内建类库）
├── litehtml/         # litehtml + gumbo（liblitehtml.a），含宿主机诊断 hosttest/
├── libwebp/          # WebP 解码（libwebp.a）
└── docs/wiki/        # 本引擎的分章节架构文档（见下）
```

## 构建

依赖 EwokOS 源码树（默认取本仓库根目录，可用 `ewokos=/path` 覆盖）：

```sh
make                    # 依次构建 libwebp → litehtml → jsnative → ewebview
make clean
```

产物安装到 EwokOS SDK 目录 `system/build_aarch64/virt/`：

| 产物 | 说明 |
| --- | --- |
| `lib/libewebview.a` | 引擎核心（C++14）+ 参考移植层（C99，按需链接） |
| `lib/liblitehtml.a` | litehtml + gumbo |
| `lib/libmario.a`    | mario VM + JS 内建类 + 四套浏览器桥 |
| `lib/libwebp.a`     | WebP 解码 |
| `include/ewebview.h`, `include/ewebview_port.h`, `include/mario/*.h` | 公开头文件 |

应用侧（如 `browser/apps/xBrowser`）链接顺序：`-lewebview -lmario -lwebp -llitehtml` 加上 EwokOS 图形与 libc 库组。

## 最小嵌入示例

```c
#include <ewebview.h>
#include <ewebview_port.h>

eweb_port_t port;
eweb_port_ewokos(&port, NULL);              /* 或填自己的平台表 */
ewebview_t* v = ewebview_create(&port);

static eweb_listener_t lis;                 /* on_frame/on_scroll/on_url/... */
eweb_listener_init(&lis);
lis.on_frame = my_on_frame;                 /* 认领帧：Blit 后 release */
ewebview_set_listener(v, &lis);

ewebview_set_viewport(v, 800, 480);
ewebview_load(v, "https://www.w3.org");

/* UI 主循环：转发输入、滚动；每拍调一次 ewebview_tick() */
ewebview_tick(v);
```

完整嵌入流程、滚动模型与移植指南见文档。

## 文档

- **[docs/wiki/](docs/wiki/README.md)** — 分章节架构文档：总体架构、线程模型、页面流水线、HTML/CSS/JS 协同、Canvas、网络/Cookie、**平台移植指南**、嵌入指南。
- [mario_js/docs/wiki/](mario_js/docs/wiki/README.md) — mario 虚拟机自身的设计文档（字节码、编译器、GC、native 扩展）。

## 许可

见 [LICENSE](LICENSE)（Apache License 2.0）。litehtml/gumbo、mario_js、libwebp 为各自上游许可的 vendored 副本。
