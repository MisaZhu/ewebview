# 第 8 章 · Canvas 2D

> 语言: [English](08-canvas.md) | **中文**

Canvas 2D 横跨三个文件，职责划分极为严格：

| 层 | 文件 | 拥有什么 |
| --- | --- | --- |
| 桥（纯 C） | `jsnative/natives/js_canvas.c` | **全部** `CanvasRenderingContext2D` 状态与几何：CTM、裁剪栈、路径构建、弧线/贝塞尔细分、even-odd 扫描线填充、折线描边、虚线、渐变与阴影求值、合成模式 |
| 画布（C++） | `ewebview/src/EWebCanvas.{h,cc}` | 一块离屏 ARGB 表面 + 把"成品图元" 1:1 映射到移植表的方法，**不做任何几何** |
| 胶合（C++） | `ewebview/src/EWebCanvasGlue.cc` | C 回调跳板、画布注册表、`drawImage` 的图片解析、合成回页面 |

```
JS: ctx.translate(x,y); ctx.bezierCurveTo(...); ctx.fill()
   │
   ▼ js_canvas.c（mario 对象 + 几何状态机，平台无关）
   CTM 变换 → 曲线细分 → 扫描线填充 → 渐变/阴影求值
   │  只发"设备坐标下的单个成品图元"
   ▼ js_canvas_callbacks_t（每个回调都是 OPTIONAL）
   EWebCanvasGlue.cc 跳板：句柄 cast 回 EWebCanvas*，转发方法
   │
   ▼ EWebCanvas 方法（1:1 映射，见 8.3）
   gfx.fill_rect / gfx.wline / gfx.stroke_bezier / font.draw_text ...
```

为什么几何全在桥里：移植表因此保持最小（第 10 章的 gfx 表只有矩形/线/圆/曲线等基础图元），引擎也不重复实现 Canvas 语义——一个新平台不需要懂 Canvas 就能跑 Canvas。

## 8.1 画布注册表与生命周期

引擎持有 `m_jsCanvases`（`std::vector<EWebCanvas*>`），key 是 `<canvas>` 元素的 **id**：

- **惰性创建**：第一次 `canvas.getContext('2d')` 时桥调 `canvas_create(id, w, h)` → `getOrCreateCanvas()` 按 id 查找、没有才 `new EWebCanvas`。同一 id 永远返回同一画布——`getContext` 幂等。
- **默认尺寸**：构造器把非正宽高钳到 HTML 默认的 **300×150**。
- **随页销毁**：`cleanupBuildResources()`（导航/换页时）调 `freeCanvases()` 统一 `delete`；析构释放位图与惰性字体。桥侧的每上下文状态由 `vm_close()` 释放。
- **匿名位图**：`ImageData` 底存、`CanvasPattern` 源、`drawImage(otherCanvas)` 的源是桥自己持有的裸 `eweb_surface_t*`（经 `bitmap_create/free/dims/data` 直接向 gfx 表申请，创建时清成全透明）。合成时以 `'@'` 开头的合成 key 表示"没有对应元素"，跳过（见 8.5）。

## 8.2 线程模型

所有 Canvas 回调**同步跑在引擎线程的一次 VM 运行里**（脚本调 `fillRect` 就是一次函数调用直达移植表）。引擎独占 VM、文档与表面，因此这里**不需要也不允许加锁**。绘制结果要等下一次标脏重排重绘（`jsMarkLayoutDirty`）才随帧上屏——Canvas 没有独立的"立即上屏"通道。

## 8.3 图元映射表

回调到达 `EWebCanvas` 时坐标已是设备空间、操作已是单个成品图元（`EWebCanvas.h` 头部注释即规范）：

| EWebCanvas 方法 | 落到移植表 |
| --- | --- |
| `fillRect` | `gfx.fill_rect` |
| `strokeRect` | 4× `gfx.wline`（`gfx.rect` 只有 1px） |
| `drawLine` | `gfx.wline`（带宽） |
| `fillCircle` / `strokeCircle` | `gfx.fill_circle` / `gfx.circle(rw=lw)` |
| `fillArc` / `strokeArc` | `gfx.fill_arc` / `gfx.arc(rw=lw)` |
| `fillRoundRect` / `strokeRoundRect` | `gfx.fill_round` / `gfx.round(rw=lw)` |
| `strokeQuadratic` / `strokeBezier` | `gfx.stroke_quadratic` / `gfx.stroke_bezier` |
| `setPixel` / `getPixel` | `gfx.set_pixel` / `gfx.get_pixel` |
| `blit` | `gfx.blit_fit_alpha` |
| `drawText` | `font.draw_text` |
| `strokeText` | 5× `font.draw_text`（4 个偏移 + 中心，描边近似） |
| `textSize` | `font.text_size` |
| `setClip` / `clearClip` | `gfx.surface_set_clip` / `gfx.surface_unset_clip` |

**曲线细分的单一来源**：桥要自己扫描线**填充**路径时，需要把曲线先拍扁成设备空间折线，这个 de Casteljau 细分不复写在桥里，而是回调移植表的 `flatten_quadratic` / `flatten_cubic`（HAL 标为 OPTIONAL）——细分实现只存在移植层一处。钩子缺失时退化为**直弦**（只回终点），填充仍然可用只是曲线变折线。

`fill_polygon` / `stroke_polygon` 两个回调**故意留 NULL**：桥的扫描线填充/折线描边路本来就会把它拆成 `fill_rect`/`draw_line` 序列，与移植层做同样的工作，不实现它们可以让回调表保持最小。

## 8.4 文字

Canvas 文字足够罕见，所以 `EWebCanvas` 的字体句柄**用到才创建**（`ensureFont()`），不写文字的画布零字体开销。`fillText`/`strokeText`/`measureText` 全部落到 `font.draw_text`/`font.text_size`；`ctx.font`、`textAlign`/`textBaseline`、`letterSpacing` 等文本样式由桥解析与求值，引擎只收到"在某处以某字号画某串"。

## 8.5 drawImage 与图片缓存复用

`drawImage(img, ...)`（3/5/9 参全支持）的图片来源经 `bitmap_from_element` 解析：

1. 元素句柄就是 DOM 桥存在 Element 实例上的 `litehtml::element*`；
2. 读它的 `src` 属性，用**容器**的 base URL 解析成绝对 URL（`resolveUrl`）；
3. 查**与 litehtml 排版共享的同一份图片缓存**（`EWebContainer::getImage`，见 5.5）——`<img>` 已下载解码的位图零成本复用；
4. 返回的 `eweb_surface_t*` **归容器所有**，桥不得释放。

构建期与存活期的容器不同：构建进行中（脚本在 `BUILD_RUN_JS` 里跑）查 `m_buildContainer`，否则查可见页的 `m_container`。

## 8.6 合成回页面

Canvas 位图不参与 litehtml 的 draw 遍历，而是在每帧渲染末尾由引擎**二次合成**（`ewebview.cc` 的 `compositeCanvases(cache, -m_engineScrollX, -m_engineScrollY)`）：

```
for each EWebCanvas in m_jsCanvases:
    id 为空或以 '@' 开头（匿名位图）→ 跳过
    el = m_doc->root()->select_one("#" + id)   ← 画布元素还在树上吗
    p  = el->get_placement()                    ← 排版后的位置
    gfx.blit(canvas位图 → 帧缓存, 目标 = origin + p.x/p.y)
```

含义与取舍：

- 合成发生在 litehtml `draw()` **之后**，所以画布像素总是盖在元素盒的背景/边框之上——近似"替换元素"的绘制次序；
- 元素被脚本从树上摘掉后 `select_one` 落空，画布自动不再显示（位图本身仍活到换页）；
- 依赖 `gfx.blit`：该回调为 NULL 时整个合成为空操作（纯标记页不受影响）；
- 每次全帧重绘都会重合成，滚动也因此天然正确——画布置于文档坐标系，随页面一起被滚动偏移。

## 8.7 支持的 JS 表面

桥按 WHATWG 规范实现了**完整的 `CanvasRenderingContext2D`**（`js_canvas.h` 头部"Supported surface"）：状态栈、全部变换（含 `getTransform`/`DOMMatrix`）、`globalAlpha`/`globalCompositeOperation`（合成模式以枚举传给引擎，不支持的模式可忽略）、填充/描边样式（颜色 | CanvasGradient | CanvasPattern）、线帽/连接/虚线、阴影四属性、全部路径方法 + `Path2D`（含 `addPath`）、`fill/stroke/clip/isPointInPath/isPointInStroke`、`fillText/strokeText/measureText` 与文本样式族、`drawImage` 三种参数形式、`createImageData/getImageData/putImageData`、线性/径向/锥形渐变、`createPattern` + `setTransform`。

已知留白：`filter` 属性接受赋值但当前忽略；无 `toDataURL`（需要图片编码器，HAL 只有解码）；无 WebGL。

## 8.8 设计要点回顾

- **几何在桥、光栅在移植层**：移植一个平台不需要理解 Canvas 语义，只要有矩形/线/圆/曲线/位图/文本这些基础图元；
- **桥内全是整数输出**：到达引擎的每个回调都是设备坐标 + `0xAARRGGBB`；
- **零额外线程/锁**：Canvas 绘制是 VM 运行的一部分，天然串行；
- **复用而非新建**：图片走容器缓存，位图走 gfx 表，文字走 font 表——Canvas 没有引入任何新的平台能力类别。
