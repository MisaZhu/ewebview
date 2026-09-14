# 第 5 章 · HTML 与 CSS：解析、排版与容器

HTML 与 CSS 子系统完全建立在 vendored 的 **litehtml + gumbo** 之上（`litehtml/`）。本章讲三件事：litehtml 的"容器"倒置依赖模型、ewebview 的容器实现 `EWebContainer` 如何把每个回调映射到移植层、以及样式从加载到绘制的完整链路。

## 5.1 litehtml 的倒置依赖：document_container

litehtml 自己**不会画任何东西**，也不会取网络资源、量字体。它定义了一个纯虚接口 `litehtml::document_container`（`litehtml/html.h`），把全部环境能力声明成约 25 个回调；解析、样式计算、布局、绘制过程中需要环境时**回调**它。ewebview 的 `EWebContainer`（`src/EWebContainer.{h,cc}`）就是这个接口的实现：

```
litehtml 引擎（平台无关）
   │  parse/render/draw 时回调
   ▼
EWebContainer : document_container（平台无关，持 eweb_port_t*）
   │  每个回调翻译成移植表调用
   ▼
eweb_port_t.gfx / font / image / net（平台实现）
```

数据流因此是：gumbo 把 HTML 文本解析成节点树 → litehtml 建 `element` 树并应用样式表 → `render(width)` 算出每个元素的几何 → `draw(hdc, ...)` 遍历时回调容器的 `draw_text`/`draw_background`/`draw_borders` → 容器调 `gfx.*`/`font.*` 画到 `eweb_surface_t`。这里的 `hdc` 就是引擎传入的表面句柄（移植层可以把它强转回原生类型）。

## 5.2 EWebContainer 回调分组

| 分组 | 回调 | 落到移植表 |
| --- | --- | --- |
| 字体 | `create_font` / `delete_font` / `text_width` / `draw_text` / `pt_to_px` / `get_default_font_size` / `get_default_font_name` | `font.create/metrics/char_width/draw_text` |
| 图形 | `draw_background` / `draw_borders` / `draw_list_marker` / `set_clip` / `del_clip` | `gfx.blit_fit_alpha/fill_rect/...`、`gfx.surface_set_clip` |
| 图片 | `load_image` / `get_image_size` | 经 host 排队下载任务；`image.decode` 在下载线程跑 |
| CSS 加载 | `import_css` / `link`（记录 media） | 经 host 排队 CSS 任务 |
| 文档服务 | `create_element`（自定义元素）、`get_media_features`、`get_language`、`get_client_rect`、`transform_text` | 视口尺寸 / 常量 |
| 交互 | `on_anchor_click` / `set_cursor` / `set_caption` | 经 host/事件队列通知 UI |

`set_caption` 是空实现（标题改由 JS 桥的 `set_title` 走）；`delete_font` 也是空——字体按 key 缓存，随容器整体销毁。

### 容器 ↔ 引擎：EWebContainerHost

容器不直接引用引擎类，而是面对一个小接口（`EWebContainer.h`）：

```cpp
class EWebContainerHost {
    virtual bool queueImageTask(const std::string& url) = 0;  // 排队图片下载
    virtual void loadCSS(const std::string& url) = 0;         // 排队 CSS 下载
    virtual void setCSSMedia(const std::string& url, const std::string& media) = 0;
    virtual void queueNavigation(const std::string& url) = 0; // <a href> 点击
    virtual bool buildAbortRequested() const = 0;             // 构建中止轮询
};
```

引擎（`EWebEngine`）实现它。容器因此可以独立测试（见 2.6 的 hosttest 用了一个更简的容器）。

## 5.3 字体管线与缓存

文字排版是 litehtml 最热的回调路径，`EWebContainer` 做了两层缓存：

1. **字体句柄缓存** `m_fonts`：key = `"family-NNpx"`。CSS family 原样传给移植层（参考 port 只有一款 CJK 系统字体，family 仅是建议），`font.create` 返回的句柄不含字号——`EWebFontInfo{font, size}` 记录请求字号，litehtml 拿到的 `uint_ptr` 就是这个缓存项的地址。`create_font` 同时经 `font.metrics` 回填 ascent/descent/height/x_height 给 litehtml 布局。
2. **字宽缓存**（8192 槽直接映射）：`text_width` 逐 UTF-8 码点求宽，每个码点查 `make_char_width_key(font,size,codepoint)` 槽位，命中即加；未命中才调 `font.char_width`。命中时的整串宽度**完全不进移植层**——这是长文排版的主要提速。

`text_width`/`create_element` 同时是**构建中止检查点**：检测到 `m_abort || host->buildAbortRequested()` 就返回哑值（解析反正要丢弃），让一次 STOP 能在毫秒级打断整页解析。

`getPerfStats()` 汇总这些计数（text_width 次数/耗时/命中率、create_font 耗时），`BUILD_RENDER_DOC` 阶段结束时打到 `sys.log`（调试构建），是排版性能回归的第一手数据。

## 5.4 CSS 的四个来源与级联

一份文档的样式来自：

1. **UA 默认样式表** `master.css`（`litehtml/include/master.css`，编译进 litehtml）：`<div>` 是块级、`<b>` 加粗这类浏览器缺省行为。引擎另支持 `ewebview_set_default_css(url)` 在首个页面前换装。
2. **外链样式表** `<link rel="stylesheet">`：解析到 `el_link` 时走 `link()` + `import_css()` 回调 → 容器解析成绝对 URL 后 `host->loadCSS(url)` 排队下载。`link()` 还会把 `media` 属性记给引擎（`setCSSMedia`）——`media="print"` 这类永不匹配的表在落地时被丢弃，避免打印样式污染屏幕渲染。
3. **内嵌样式块** `<style>`：litehtml 自己处理。
4. **内联样式** `style="..."` 属性：同上。

**异步是这里的核心设计**：`import_css` 回调拿不到 CSS 文本（下载没完成），它只记 URL；CSS 文本经下载线程到达后由引擎 `loadCSSContent()` 灌进 context 的 master 样式集，置脏标记，下一轮引擎循环统一重算样式。同一份 URL 用 `m_seenCssUrls` 去重。

媒体查询（`@media`）依赖 `get_media_features()` 的返回：视口宽高取容器客户区，`device 640x480`、`resolution 96`、`color 8` 是常量（可按 port 调整）。

## 5.5 图片管线：延迟、下载、挂载

`load_image(src, baseurl, redraw_on_ready)` 的三条路径：

- **已在缓存** `m_images`：引用计数 +1，直接返回；
- **构建期**（`m_defer_image_load == true`）：只把 URL 记入 `m_pending_image_urls`，不发任务——半成品页不配消耗带宽；`BUILD_SWAP_DOC` 后置 `m_flushDeferredImages`，引擎循环第 8 步调 `flushPendingImages()` 统一排队；
- **存活期**：`host->queueImageTask(url)` 立即排队。

图片**解码在下载线程**完成（`decodeImageData` → `image.decode`，纯堆操作），引擎线程只做 O(1) 的挂载（`mountImage` 进 `m_images`）。`get_image_size` 在布局时查缓存：没下载完给 0x0，下载完重排时拿真实尺寸。`draw_background` 用 `gfx.blit_fit_alpha` 把背景图缩放混排进背景框。

同一缓存还被 Canvas 桥复用：`drawImage(<img>)` 经 `bitmap_from_element` 读元素 `src`、解析成绝对 URL、查这份缓存（见第 8 章）。

## 5.6 自定义元素：eweb_el_input

litehtml 对 `<input>` 只有通用元素。`create_element` 钩子拦截 `type="text"` / `type="button"`，构造 `eweb_el_input`（`src/eweb_el_input.{h,cc}`）——一个**替换元素**（`is_replaced()` 返回真）：自带固有尺寸（`get_content_size`）、自己 `draw()`（经 port 的 gfx/font 画边框、底色、文本、按钮文字居中），点击有反馈（`on_click`）。它从 widget++ 时代的 `el_input` 移植而来，全部绘制走移植表，不认识 EwokOS。

这是 litehtml 扩展点的一个范例：**任何需要自定义排印行为的标签都可以在 `create_element` 里拦截**。

## 5.7 绘制：draw 阶段如何落像素

`engineRenderFrame()` 里，引擎对当前文档调 `m_doc->draw(hdc, x, y, clip)`（`drawPageToCacheLocked`），litehtml 深度遍历元素树，每个元素回调容器：

- 背景色/背景图 → `draw_background` → `gfx.fill_rect` / `gfx.blit_fit_alpha`（含 `border-radius` 裁剪由 `set_clip` 配合）；
- 边框 → `draw_borders` → `gfx.fill_rect`（四边）等；
- 文本 → `draw_text` → `font.draw_text(surface, x, y, utf8, font, size, argb)`；
- 列表 marker → `draw_list_marker` → 画圆点/数字文本。

颜色一律 `0xAARRGGBB`（`web_color_to_argb` 从 litehtml 的 `web_color` 转换），坐标为文档坐标系下的设备像素。draw 阶段**纯绘制**——所有几何在 render 阶段已定，这使得重绘（滚动、内容脏）不重排。

## 5.8 HTML/CSS 的已知边界

- `<script src="...">` **外链脚本不加载**（`extract_scripts` 直接丢弃，见第 6 章）；
- litehtml 本身是 CSS2.1 + 部分 CSS3（flex 有 `flex_layout`，无 grid）；
- `position:fixed` 参与命中测试（`get_element_by_point` 的 client 坐标对），其余行为从简；
- 表单仅 `text`/`button` 两种 `<input>` 是替换元素，其余按普通标签渲染；
- 无 iframe/worker/popup——`window.postMessage` 接受调用但什么都不做（见第 7 章"已知留白"）。

这些边界大多是有意的取舍：引擎目标是嵌入式设备上的实用渲染，不是完整 Web 平台。
