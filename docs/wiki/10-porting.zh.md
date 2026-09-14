# 第 10 章 · 平台移植指南

> 语言: [English](10-porting.md) | **中文**

ewebview 核心不认识任何操作系统：它只把页面渲染进一块抽象的 ARGB8888 内存画布（`eweb_surface_t`），一切平台能力经 **`eweb_port_t`**（`porting/include/ewebview_port.h`）注入。移植一个平台 = 填六张回调表。本章逐表讲契约，再对照参考移植 `porting/src/ewokos/port_ewokos.c` 给出落地步骤。

## 10.1 移植层的设计规则

- **`eweb_port_t` 是六张表的值包**：`gfx / font / image / net / clock / sys`，`ewebview_create()` 时**按值拷贝**，调用者随后可以释放自己的副本。
- **每张表自带 `ud` 指针**，原样回传给每个回调——移植层借此 dispatch 到自己的上下文，核心从不解释它。
- **不透明句柄**：`eweb_surface_t` / `eweb_font_t` 对核心是不透明的；具体类型由移植自定（EwokOS：`graph_t*` / `font_t*`）。嵌入者若**就是**移植层，可经 `surface_native()` 拿回具体指针做零拷贝上屏（widget++ 就是这么把帧 blit 进窗口的，见第 11 章）。
- **颜色恒为 `0xAARRGGBB`**，坐标恒为设备像素整数（原型注明 float 的除外）。
- **每个回调都是 OPTIONAL，除非标注 REQUIRED**：NULL 钩子让对应能力**降级**而不是崩溃——移植可以增量点亮。

## 10.2 六张表总览与线程矩阵

| 表 | 调用线程 | REQUIRED 成员 | 整表缺失时 |
| --- | --- | --- | --- |
| `gfx` | **仅引擎线程** | `surface_new` / `surface_free` / `surface_dims` / `blit` / `blit_fit_alpha` | 无法渲染 |
| `font` | **仅引擎线程** | `create` / `destroy` / `metrics` / `char_width` / `draw_text` | 无法排版文字 |
| `image` | **下载线程** | 无（有图片需求才算 REQUIRED） | `<img>`/背景图保持 0×0 |
| `net` | **仅下载线程** | `request`+`free_response`（http 页）、`read_file`（file 页） | 无网络/本地文件加载 |
| `clock` | **引擎+下载线程都调** | `tic_ms` | 定时器/去抖/预算全部失灵 |
| `sys` | 引擎线程 | 无 | 日志丢弃、句柄检查只剩活性标记 |

最小可用集 = **gfx + font + clock**（页面可排版可绘制）；其余按需增量添加。

## 10.3 gfx 表：表面与图元

**表面生命周期**（REQUIRED）：`surface_new(w,h)` 分配画布（帧池、`<canvas>` 底存、匿名位图都用它）；`surface_free` 必须容忍 NULL；`surface_dims` 供帧池复用前校验尺寸。

**直接像素访问**：`surface_pixels` 返回 `w*h` 的 `uint32_t` 缓冲——Canvas 的 ImageData 读写走这条快路径；**可以返回 NULL**，核心自动退到 `get_pixel`/`set_pixel`。`surface_native` 返回平台原生句柄（核心永不解引用）。

**图元分组**：矩形/线（`fill_rect`/`rect`/`line`/`wline`）、圆与弧（`circle`/`fill_circle`/`arc`/`fill_arc`，角度为弧度、自 +x 顺时针）、圆角矩形（`round`/`fill_round`）、曲线描边（`stroke_quadratic`/`stroke_bezier`）、像素（`set_pixel`/`get_pixel`）、位块（`blit` 1:1 拷贝；`blit_fit_alpha` 缩放 + 0..255 透明度混合，背景图与 Canvas 合成都是它）。

**曲线细分**（OPTIONAL）：`flatten_quadratic`/`flatten_cubic` 把 de Casteljau 细分结果写成 interleaved float 折线（起点除外），供桥扫描线**填充**曲线（见 8.3）。留 NULL 时核心退化为直弦。`surface_set_clip`/`surface_unset_clip` 是矩形裁剪（`border-radius` 与 Canvas `clip()` 都依赖它）。

## 10.4 font 表：句柄不含字号

- `create(family)` 返回字体句柄；**字号不烘焙进句柄**，每次测量/绘制调用都传 `size`——一个句柄服务全部字号。CSS family 只是建议（参考移植无视它，恒 `font_new("system-cn")`）。
- `metrics(f, size)` 回填 ascent/descent/height/x_height——litehtml 排版的行高全部来自这里。
- `char_width(f, size, codepoint)`（UTF-32 码点）是**整个排版最热的回调**；引擎侧有 8192 槽字宽缓存兜底（见 5.3），但移植实现本身也应尽量是 O(1) 查表。
- `text_size`（OPTIONAL）：整串测量；缺省时核心逐码点求和 `char_width`。
- `draw_text(s, x, y, utf8, f, size, color)`：左上角基线定位。

## 10.5 image / net / clock / sys

**image.decode(data, size)**：把 png/jpeg/gif/svg/webp/... 字节解码成**新的** ARGB8888 表面（失败返回 NULL；核心用 `gfx.surface_free` 释放）。它在**下载线程**上跑（纯堆活），不得触碰引擎线程专属状态。

**net 表**：

| 成员 | 契约 |
| --- | --- |
| `request` | 发**一次** http(s) 请求，**不得自动跟随重定向**（核心自己逐跳跟随以重算 Cookie 作用域，见 9.2）。`req_headers` 带着核心为本跳构建的 Cookie 头。收到响应（哪怕 4xx/5xx）返回 true 并填 `resp`；传输层失败返回 false |
| `free_response` | 释放 `resp` 的 body/headers/native；与 `request` 成对 REQUIRED。响应内存在此调用前一直有效 |
| `read_file` | `file://`：按绝对路径读文件，返回 **malloc'd** 缓冲（核心 `free()`） |
| `resolve_resource` | OPTIONAL：移植私有 scheme（EwokOS 的 `res://`）解析成真实路径 |

**clock 表**：`tic_ms` 单调毫秒钟（REQUIRED，双线程调用——必须线程安全），驱动 CSS/JS 定时器、布局去抖、VM 预算；`sleep_ms` OPTIONAL（缺省时核心用 pthread 条件变量计时等待）。

**sys 表**：`ptr_sane` 是**不解引用**的指针堆内合理性检查，JS 桥在读元素活性标记之前先过它（挡伪造句柄，见 6.6）；`log` 输出核心日志行（文本已带 `[js]` 等来源前缀）。

## 10.6 参考移植对照：port_ewokos.c 与 port_sdl2.c

仓库内置两份完整的参考移植，都是纯 C99、结构都是"一组 `ek_*` 静态函数 + `eweb_port_*()` 填表"：

- **`porting/src/ewokos/port_ewokos.c`**（约 400 行）：EwokOS 原生图形栈；
- **`porting/src/sdl2/port_sdl2.c`**（约 1400 行）：桌面端 SDL2，是**默认**构建的 port，比 EwokOS port 多填了全部 OPTIONAL 钩子（`surface_pixels`、`flatten_quadratic/cubic`、`text_size` 等）并支持 HiDPI。

| HAL 成员 | EwokOS 实现 | SDL2 实现 |
| --- | --- | --- |
| `eweb_surface_t` | `graph_t*`（plain cast） | `sdl_surf_t{ SDL_Surface(ARGB8888) + SDL_Renderer }` |
| `eweb_font_t` | `font_t*` | `sdl_font_t{ family + 按字号缓存的 TTF_Font* }` |
| gfx.* | `graph_*` 族一一对应（`G(s)`/`SG(g)` 宏强转；`surface_native` 返回 `graph_t*`） | `SDL_FillRect`/`SDL_BlitSurface`/`SDL_BlitScaled` + SDL2_gfx 图元（line/box/circle/arc/pie/rounded/bezier） |
| font.* | `font_new("system-cn")` + `font_metrics`/`font_char_width`/`font_draw_text` | SDL2_ttf（句柄不含字号，每字号缓存一个 `TTF_Font*`） |
| image.decode | SVG 嗅探 → `graph_image_new_from_data(AUTO)` → WebP 走 `libwebp` 兜底 | `IMG_Load_RW`(SDL2_image) + `ConvertSurfaceFormat(ARGB8888)`，WebP 走 `libwebp` |
| net.request | tinyhttpsc/BearSSL；`SetTimeout(10000)` + **`SetMaxRedirections(0)`** | 同一份 tinyhttpsc/BearSSL；同样 **`SetMaxRedirections(0)`** |
| net.read_file | `vfs_readfile` | 标准 C `fopen`/`fread` |
| net.resolve_resource | `x_get_res_name`（`res://`） | `<program-dir>/res/<name>`（经 `SDL_GetBasePath`） |
| clock | `sys_tic_ms(0)` / `proc_usleep` | `clock_gettime(CLOCK_MONOTONIC)` / `SDL_Delay` |
| sys.log | `klog` | `SDL_Log` |
| sys.ptr_sane | `ewok_ptr_in_heap` | 非 NULL 启发式（桌面无廉价堆归属检查，OPTIONAL，核心退化为只信活性标记） |

**HiDPI**：`eweb_port_sdl2_set_dpr(float)` 让 `surface_new` 按 `逻辑尺寸 × dpr` 分配设备像素，每个 draw/font 回调把逻辑坐标放大——布局留在 CSS 像素，光栅化跑在原生设备分辨率。嵌入者需在 `ewebview_create()` 前、UI 线程上调它（sdlbrowser 从 `SDL_GetBasePath` 侧探测）。

**按需链接**：两份 port 对象都编进 `libewebview.a`（由 `PORTING` 决定编哪份），但只有嵌入者真的调用了 `eweb_port_ewokos()` / `eweb_port_sdl2()` 才会被链接器拉入——外部平台链接 `libewebview.a` 并提供自己的移植时，**完全不会引入** graph/font/SDL2/tinyhttpsc 符号（Makefile 头部注释即此设计）。注意 `eweb_port_sdl2()` **未在 `ewebview_port.h` 里声明**（公开头只声明了 `eweb_port_ewokos()`），sdl2 嵌入者需自行 `extern` 声明。

## 10.7 新平台落地顺序

按依赖从底向上，每一步都有可验证的里程碑：

1. **`eweb_port_init()` 清零**，填 `clock.tic_ms` —— 引擎节拍、定时器、看门狗预算全依赖它；
2. **gfx 最小集**：`surface_new/free/dims/clear` + `fill_rect` + `blit` + `blit_fit_alpha`；
3. **font 全集**（5 个 REQUIRED 成员）——到这一步，`ewebview_load("file:...")` 加载的纯文本页已能排版上屏；
4. **`net.read_file`** → `file://` 本地页加载；
5. **`net.request`/`free_response`**（记得关自动重定向）→ http(s) 页面与 CSS/图片下载；
6. **`image.decode`** → `<img>` 与背景图；
7. **按需补 OPTIONAL**：`surface_pixels`（Canvas ImageData 快路径）、`surface_set_clip`（圆角/clip）、`flatten_*`（曲线填充精度）、`text_size`、`sleep_ms`、`resolve_resource`、`sys.ptr_sane`；
8. **`sys.log` 尽早接**——引擎的构建阶段耗时、字宽缓存命中率、任务队列活动全在日志里，是移植期调试的第一手数据。

每一步缺失时的降级行为都写在 `ewebview_port.h` 对应成员的注释里——HAL 的注释就是契约。

## 10.8 宿主机预验证：sdl2 port + sdlbrowser

移植出问题时要先分清"litehtml 集成问题"还是"平台问题"。默认的 `PORTING=sdl2` 宿主机构建（`make`，见第 2 章）用原生编译器把引擎 + `port_sdl2` + `bin/sdlbrowser` 编成一个桌面可执行——**不需 QEMU / 目标硬件**就能跑真实页面。排版/脚本/网络疑问先在宿主机 sdlbrowser 上复现（它走的是同一份平台无关核心），再决定查移植层还是核心：若宿主机正常而目标平台异常，问题几乎总在目标 port 的某张表里。

> 旧版的 `litehtml/hosttest` 诊断 harness 已移除；它的“宿主机复现排版”职责现在由 sdl2 port + sdlbrowser 承担，且覆盖面更完整（含 JS/网络/Canvas，不只是排版）。

## 10.9 移植检查清单

- [ ] `tic_ms` 单调、线程安全（双线程调用）
- [ ] `surface_free` 容忍 NULL；`surface_dims` 可用于帧池校验
- [ ] `blit_fit_alpha` 支持缩放 + alpha 混合
- [ ] `char_width` 是 O(1) 级别（排版热路径）
- [ ] `metrics` 回填四个行高度量
- [ ] `net.request` **不自动跟随重定向**；响应内存活到 `free_response`
- [ ] `image.decode` 不触碰引擎线程状态（跑在下载线程）
- [ ] 句柄强转宏集中定义（参考移植的 `G/SG/FT/EF` 模式），不散落 cast
- [ ] 全程在真机（或宿主机 sdlbrowser）跑测试页：sdlbrowser 内置 `res://html/` 起步页，EwokOS 仓的 `browser/data/test/html/` 有更全的 DOM/事件/表单/定时器/canvas/storage 用例（见第 11 章嵌入侧）
