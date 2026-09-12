# 第 10 章 · 平台移植指南

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

## 10.6 参考移植对照：port_ewokos.c

EwokOS 移植是纯 C99、不足 400 行，结构就是"一组 `ek_*` 静态函数 + `eweb_port_ewokos()` 填表"：

| HAL 成员 | EwokOS 实现 |
| --- | --- |
| gfx.* | `graph_*` 族一一对应（`G(s)`/`SG(g)` 宏做句柄强转；`surface_native` 直接返回 `graph_t*`） |
| font.* | `font_new("system-cn")` + `font_metrics`/`font_char_width`/`font_draw_text` |
| image.decode | SVG 嗅探（`ek_looks_like_svg`）→ `graph_image_new_from_data(AUTO)` → WebP 走 `libwebp` 兜底 |
| net.request | tinyhttpsc/BearSSL；`SetTimeout(10000)` + **`SetMaxRedirections(0)`**（重定向交给核心） |
| net.read_file | `vfs_readfile` |
| net.resolve_resource | `x_get_res_name`（`res://`） |
| clock | `kernel_tic_ms(0)` / `proc_usleep` |
| sys | `ewok_ptr_in_heap` / `klog` |

**按需链接**：`port_ewokos.o` 编进 `libewebview.a`，但只有嵌入者真的调用了 `eweb_port_ewokos()` 才会被链接器拉入——外部平台链接 `libewebview.a` 并提供自己的移植时，**完全不会引入** graph/font/tinyhttpsc 符号（Makefile 头部注释即此设计）。

`porting/src/sdl2/` 目录目前是 EwokOS 移植的**拷贝脚手架**（内容与 `port_ewokos.c` 相同，Makefile 中对应行被注释），是留给桌面 SDL2 移植的占位。

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

## 10.8 宿主机预验证：litehtml/hosttest

移植出问题时要先分清"litehtml 集成问题"还是"平台问题"。`litehtml/hosttest/build.sh` 用宿主机编译器（macOS clang）把 litehtml + gumbo + 一个 stub 容器编成 `lhtest`，**不依赖任何移植层**——排版疑问先在宿主机上复现，再决定查哪一层。

## 10.9 移植检查清单

- [ ] `tic_ms` 单调、线程安全（双线程调用）
- [ ] `surface_free` 容忍 NULL；`surface_dims` 可用于帧池校验
- [ ] `blit_fit_alpha` 支持缩放 + alpha 混合
- [ ] `char_width` 是 O(1) 级别（排版热路径）
- [ ] `metrics` 回填四个行高度量
- [ ] `net.request` **不自动跟随重定向**；响应内存活到 `free_response`
- [ ] `image.decode` 不触碰引擎线程状态（跑在下载线程）
- [ ] 句柄强转宏集中定义（参考移植的 `G/SG/FT/EF` 模式），不散落 cast
- [ ] 全程在真机跑 `data/test/html/` 下的用例页（见第 11 章嵌入侧）
