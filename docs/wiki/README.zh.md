# ewebview 架构 Wiki

> 语言: [English](README.md) | **中文**

欢迎来到 **ewebview** 的设计文档。这套 Wiki 面向需要理解、嵌入或移植本引擎的读者，从整体架构一路讲到 HTML/CSS/JS 三者的协同关系、线程模型与平台移植方法。

ewebview 是一个自包含的嵌入式 Web 引擎：核心只认一块抽象的 ARGB8888 内存画布和一组平台回调表（`eweb_port_t`），不认识任何具体操作系统。HTML 排版（litehtml + gumbo）、JavaScript（mario VM）与 Canvas 2D 都是引擎内部组件；图形、字体、图片解码、网络、时钟则全部由"移植层"注入。

仓库内置两套参考移植：**`sdl2`**（默认，桌面端，配 `bin/sdlbrowser` 演示壳）与 **`ewokos`**（EwokOS，xBrowser 即构建其上）。

---

## 阅读顺序

建议按顺序阅读，每一章建立在前一章的概念之上：

| 章节 | 标题 | 内容简介 |
| --- | --- | --- |
| 第 1 章 | [总体架构](01-overview.md) | 引擎是什么、分层结构、模块地图、一次页面加载的数据流 |
| 第 2 章 | [目录结构与构建系统](02-build.md) | 五个子库的构建顺序、宿主机/交叉两种构建模式、SDK 安装、链接顺序、调试开关 |
| 第 3 章 | [线程模型与帧交付](03-threading.md) | 引擎线程 / 下载线程池 / UI 线程，命令与事件队列，帧池与背压 |
| 第 4 章 | [页面加载流水线](04-page-pipeline.md) | build 状态机六个阶段、构建中止、样式分片、渐进式绘制 |
| 第 5 章 | [HTML 与 CSS：解析、排版与容器](05-html-css.md) | gumbo → litehtml → document_container 回调如何映射到移植层 |
| 第 6 章 | [JavaScript：mario VM 集成与脚本调度](06-js-vm.md) | 脚本抽取、VM 生命周期、运行预算看门狗、document.write 重解析 |
| 第 7 章 | [JS 桥接层：DOM / Event / Web](07-js-bridges.md) | 四套纯 C 桥、元素句柄活性、事件传播、BOM 表面 |
| 第 8 章 | [Canvas 2D](08-canvas.md) | 桥内几何状态机、离屏画布注册表、合成回页面 |
| 第 9 章 | [网络、Cookie 与存储](09-network-cookies.md) | 子资源任务队列、重定向与 Cookie 作用域、localStorage |
| 第 10 章 | [平台移植指南](10-porting.md) | `eweb_port_t` 六张表逐一详解、REQUIRED/OPTIONAL、ewokos/sdl2 两份参考移植对照、新平台落地步骤 |
| 第 11 章 | [嵌入指南](11-embedding.md) | C API 用法、监听器、滚动模型、sdlbrowser/WidgetWebview/xBrowser 实例 |

---

## 一分钟理解 ewebview

```
   URL (http/https/file/res)
        │
        ▼  下载线程池：net.request / net.read_file / image.decode   ── 第 9 章
  HTML 字节流 + CSS + 图片
        │
        ▼  gumbo 解析 → litehtml 文档树 + 样式表 + 排版            ── 第 5 章
  litehtml::document（元素树）
        │
        ▼  <script>（内联 + 外链 src 下载）交给 mario VM；DOM/Event/Web/Canvas 桥回写文档 ── 第 6、7、8 章
  布局后的文档 + 画布位图
        │
        ▼  引擎线程经 gfx/font 回调光栅化到帧池表面                ── 第 3 章
  ARGB8888 帧（eweb_surface_t*，EwokOS 上即 graph_t*、SDL2 上包着 SDL_Surface*）
        │
        ▼  on_frame() 交给嵌入者 blit 进自己的窗口                 ── 第 11 章
   屏 幕
```

所有平台相关的能力都从右侧的 **`eweb_port_t` 移植表**（第 10 章）进入引擎。
