# 第 1 章 · 总体架构

## 1.1 ewebview 是什么

ewebview 是一个**自包含（self-contained）的嵌入式 Web 引擎**：给它一个 URL，它在后台线程里完成下载、解析、排版、脚本执行与光栅化，把结果以一块 ARGB8888 内存画布（`eweb_surface_t`）的形式交给嵌入者。嵌入者只需要把这块画布 blit 进自己的窗口。

它与 Chromium/WebKit 类引擎的根本区别在于**依赖方向**：

```
传统浏览器：引擎 -> 直接调用 OS 图形/网络/字体 API
ewebview：  引擎 -> eweb_port_t 回调表 -> 由"移植层"决定调用什么
```

核心代码（`ewebview/src/`）**不出现任何平台符号**——没有 `graph_t`、没有 socket、没有文件系统调用。它只认识：

- `eweb_surface_t*`：不透明画布句柄（EwokOS 上恰好是 `graph_t*`）；
- `eweb_font_t*`：不透明字体句柄（EwokOS 上是 `font_t*`）；
- `eweb_port_t` 里的六张函数表：gfx / font / image / net / clock / sys。

这一契约声明在 [`ewebview/porting/include/ewebview_port.h`](../../ewebview/porting/include/ewebview_port.h)，公开 API 在 [`ewebview/include/ewebview.h`](../../ewebview/include/ewebview.h)。

## 1.2 设计目标

1. **可移植**：换平台 = 新写一份 port（填六张表），核心零改动。参考实现 `port_ewokos.c` 约 400 行。
2. **可增量**：除 gfx/font/clock 外的每张表都是 OPTIONAL，留空则对应能力降级而不是崩溃——可以先点亮排版，再补网络，再补图片。
3. **UI 永不阻塞**：下载、解析、排版、脚本都在引擎线程与下载线程上；UI 线程只投递命令、收事件、blit 帧。
4. **零拷贝帧交付**：帧表面以所有权转移的方式跨线程移交，嵌入者甚至可以把表面强转回平台原生类型直接 blit（EwokOS 上就是这么做的）。
5. **面向受限环境**：全部静态链接、无 C++ 异常/RTTI 依赖（litehtml 以 `-fno-rtti` 构建）、脚本运行带看门狗，恶意死循环无法锁死引擎。

## 1.3 分层结构

```
┌────────────────────────────────────────────────────────────┐
│ 嵌入者（Embedder）：xBrowser / WidgetWebview / 你的应用       │  UI 线程
├────────────────────────────────────────────────────────────┤
│ 公开 C API：ewebview.h                                      │
│   create/load/stop/reload/set_viewport/post_event/scroll/   │
│   tick/release_frame + eweb_listener_t 回调表               │
├────────────────────────────────────────────────────────────┤
│ 引擎核心（libewebview.a，平台无关 C++14）                     │  引擎线程
│   ewebview.cc        引擎循环、build 状态机、帧池、下载调度    │  下载线程
│   EWebContainer.cc   litehtml document_container 实现        │
│   EWebJs.cc          mario VM 生命周期 + DOM/Event/Web 桥回调 │
│   EWebCanvas(Glue)   <canvas> 离屏画布 + Canvas 桥胶水        │
│   EWebCookies.cc     进程级 CookieJar                        │
├───────────────┬──────────────────────────┬─────────────────┤
│ litehtml+gumbo│ mario VM + jsnative 桥    │ libwebp          │
│ (liblitehtml.a│ (libmario.a)             │ (libwebp.a)      │
├───────────────┴──────────────────────────┴─────────────────┤
│ 移植层 HAL：eweb_port_t（gfx/font/image/net/clock/sys）       │  平台
│   参考实现：port_ewokos.c = graph/font/tinyhttpsc/vfs/...    │
└────────────────────────────────────────────────────────────┘
```

- **litehtml + gumbo**（vendored 于 `litehtml/`）：HTML 解析与 CSS 排版引擎。gumbo 是 HTML5 解析器（C），litehtml 在其上构建元素树并执行样式计算与布局（C++）。
- **mario VM**（`mario_js/` 子模块）：字节码虚拟机 + JavaScript 前端，纯 C、无依赖，内存分配与输出都由宿主注入。
- **jsnative**（`jsnative/natives/`）：四套浏览器桥——`js_dom`（Document/Element）、`js_event`（Event/EventTarget）、`js_web`（window/location/storage/XHR/fetch）、`js_canvas`（Canvas 2D），同样纯 C、平台无关。
- **libwebp**：WebP 解码；其余图片格式（png/jpeg/gif/tga/svg）由移植层自行接入（EwokOS 走 graph_image/plutosvg）。

## 1.4 核心内部模块地图

引擎核心只有一个对外不透明类型 `ewebview_t`，其内部是 `eweb::EWebEngine`（见 `src/EWebInternal.h`）：

| 成员/模块 | 职责 |
| --- | --- |
| `m_port` | 创建时按值拷贝的 `eweb_port_t`，一切平台调用的入口 |
| `m_doc` / `m_container` | 当前**可见页**的 litehtml 文档与容器 |
| `m_buildDoc` / `m_buildContainer` / `m_buildPhase` | **构建中**的新页与 build 状态机 |
| `m_browser_context` / `m_buildContext` | 两份 litehtml context（样式表缓存），双缓冲交替使用 |
| `m_cmdQueue` / `m_uiQueue` | UI→引擎命令队列、引擎→UI 事件队列 |
| `m_taskQueue` / `m_resultQueue` | 子资源下载任务与结果队列（下载线程） |
| `m_freeFrames` / `m_pendingFrame` | 视口帧池（双缓冲）与待认领帧 |
| `m_jsVm` + JS 状态 | mario VM、脚本列表、运行预算、mutation 日志 |
| `m_jsCanvases` | 每个 `<canvas>` 的离屏画布注册表 |
| `EWebCookieJar`（进程单例） | HTTP 与 document.cookie 共享的 Cookie 仓库 |

两个关键的内部接口把核心解耦：

- **`EWebContainerHost`**（`EWebContainer.h`）：容器在 litehtml 解析/排版期间需要引擎做的事——排队图片任务、加载 CSS、记录 media、排队导航、查询构建中止标志。容器因此不直接引用引擎类型。
- **`js_*_callbacks_t`**（`jsnative/natives/*.h`）：四套 JS 桥对引擎的回调表，桥是纯 C，不认识 litehtml；`EWebJs.cc`/`EWebCanvasGlue.cc` 里的静态成员函数把这些回调落到 litehtml 文档与画布上。

## 1.5 一次页面加载的数据流

```
ewebview_load(url)                       [UI 线程]
   │  投递 ECMD_NAVIGATE（立即返回）
   ▼
engineLoop() 取命令 → engineNavigate()   [引擎线程]
   │  排队主文档任务 EWEB_TASK_HTML
   ▼
taskLoop() 取任务                        [下载线程]
   │  net.request() 拉取字节，Cookie 逐跳处理，图片顺带解码
   ▼
pushResult() → 引擎 processResults()
   │  HTML 到手：extract_scripts() 剥离 <script>
   ▼
build 状态机（第 4 章详述）：
   PRELOAD_CSS → CREATE_DOC(gumbo+litehtml 解析) → RUN_JS(可选)
   → RENDER_DOC(样式+布局) → SWAP_DOC(新页上屏) → 余下脚本渐进执行
   │
   ▼
engineRenderFrame()：gfx/font 回调光栅化视口 → EUET_FRAME
   │
   ▼
ewebview_tick() → listener.on_frame(frame)   [UI 线程]
   嵌入者 blit 上屏，旧帧 ewebview_release_frame() 归还帧池
```

此后页面进入**存活期**：JS 定时器（`jsPollTimers`）、鼠标事件（`jsDispatchMouseEvent` → DOM 事件传播 → 可能改 DOM → 标脏 → 重排 → 重绘新帧）、滚动、`<canvas>` 动画，都由引擎循环持续驱动，UI 线程每拍调一次 `ewebview_tick()` 收事件即可。

## 1.6 HTML、CSS、JS 三方协同（总览）

三者的分工与交汇点（后续章节展开）：

- **HTML（gumbo/litehtml）** 产出元素树；`<script>` 在解析前被 `extract_scripts()` 剥出交给 JS，`<link>`/`<style>` 由容器转交 CSS 子系统。
- **CSS（litehtml stylesheet + master.css）** 决定元素树如何排版与绘制；绘制本身不直接碰像素，而是回调容器的 `draw_text`/`draw_background`/`draw_borders`，再经 `eweb_port_t.gfx/font` 落盘到画布。
- **JS（mario VM + 四套桥）** 通过 DOM 桥**读写同一棵 litehtml 元素树**（元素句柄就是 `litehtml::element*`），通过 Event 桥接收输入事件，通过 Web 桥读写 Cookie/存储/发起导航，通过 Canvas 桥向 `<canvas>` 位图画图。JS 的每次 DOM 变更都会标脏布局，由引擎在适当时机重排重绘——这就是"改了 DOM 屏幕就变"的完整链路。

细节：第 5 章（HTML/CSS）、第 6 章（JS 调度）、第 7 章（JS 桥）。

## 1.7 与其他组件的关系

- **WidgetWebview**（`browser/libs/widget++/src/WidgetWebview/`）：把 ewebview 包成 widget++ 控件的参考嵌入者，约 500 行——证明核心之外不再需要任何 Web 逻辑。
- **xBrowser**（`browser/apps/xBrowser/`）：完整浏览器应用 = WidgetWebview + 地址栏/状态栏/历史。
- **Qt 侧**（`projects/qt/apps/qbrowser` 等）：同一份 SDK 头文件与库也可被 Qt 应用复用（litehtml 直接渲染到 `graph_t`）。
- 测试页：`browser/data/test/html/*.html`（DOM、事件、表单、定时器、canvas、storage 等）。
