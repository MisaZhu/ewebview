# 第 11 章 · 嵌入指南

本章面向把 ewebview 装进自己应用的读者：C API 的调用次序、帧与滚动的所有权契约、每个监听器钩子的语义，最后解剖参考实例 `WidgetWebview` + `xBrowser`。全部 API 在 `ewebview/include/ewebview.h`。

## 11.1 最小嵌入序列

```c
#include <ewebview.h>
#include <ewebview_port.h>

/* 1. 移植表：用参考移植，或按第 10 章自填 */
eweb_port_t port;
eweb_port_init(&port);
eweb_port_ewokos(&port, NULL);          /* EwokOS 参考移植 */

/* 2. 建引擎（引擎线程立即启动并驻车）；gfx/font/clock 缺失会返回 NULL */
ewebview_t* v = ewebview_create(&port);

/* 3. 装监听器（只填关心的钩子，其余留 NULL） */
eweb_listener_t lis;
eweb_listener_init(&lis);
lis.ud       = my_ctx;
lis.on_frame = my_on_frame;             /* 帧交付，唯一真正必须的钩子 */
ewebview_set_listener(v, &lis);

/* 4. 视口 + 导航 */
ewebview_set_viewport(v, win_w, win_h);
ewebview_load(v, "https://example.com/");

/* 5. UI 主循环：每个 tick（如 30Hz 定时器）泵一次 */
ewebview_tick(v);                       /* 所有 listener 回调在这里触发 */

/* 6. 输入与滚动（都是排队即返回） */
ewebview_post_event(v, &ev);
ewebview_scroll(v, x, y);

/* 7. 析构：先归还所有收养的帧，再销毁 */
ewebview_release_frame(v, held_frame);
ewebview_destroy(v);
```

## 11.2 线程契约（再强调）

- **UI → 引擎**的每个 API（`load/stop/reload/set_viewport/scroll/post_event/set_js_enabled`）都只是**往命令队列塞一条命令并立即返回**——窗口永远不会被一次 fetch/parse 卡住；
- **引擎 → UI**的所有回调都在 **UI 线程上、`ewebview_tick()` 内部**触发。因此回调里**绝不能阻塞**（阻塞 tick 就是阻塞整个浏览器 UI），也不要在回调里重入引擎 API 以外的引擎状态；
- 引擎线程与下载线程是引擎私有的（第 3 章），嵌入者永远不需要碰它们。

## 11.3 帧交付与所有权

```c
void my_on_frame(void* ud, eweb_surface_t* frame,
                 int frame_scroll_x, int frame_scroll_y, int doc_w, int doc_h);
```

- **所有权随回调转移**：嵌入者收养这帧（通常当作显示缓存，在自己的 repaint 里 blit 它），用完后经 `ewebview_release_frame()` 归还——一般在收养下一帧之前还上一帧；
- **背压**：帧未归还期间引擎不会再往这块表面画——天然限制在帧数，无跨线程像素拷贝、无每帧分配；
- **零拷贝**：嵌入者与移植同平台时，帧句柄就是平台原生表面。EwokOS 上 `frameGraph()` 直接把 `eweb_surface_t*` 强转回 `graph_t*` blit 进窗口（等价于 `surface_native()` 的快捷方式）；
- `doc_w/doc_h` 是文档完整尺寸，用来给滚动条定比例。

## 11.4 滚动模型：位移 blit + 权威回执

滚动是嵌入者与引擎的**双人舞**，目的是让拖滚/滚轮零延迟：

```
用户滚动手势
  │  1. 嵌入者按最后已知文档几何 clamp 目标偏移
  │  2. 立即移动自己的 live offset → onRepaint 把缓存帧
  │     按 (frameScroll - liveScroll) 位移 blit（页面即时跟手）
  │  3. ewebview_scroll(x, y) 通知引擎
  ▼
引擎线程：更新滚动偏移 → 重绘暴露出的条带 → 触发页面 scroll 处理器
  │  4. on_frame 带来按新偏移渲染的完整帧
  │  5. 权威事件 on_scroll(x, y, doc_w, doc_h)：换页归零、
  │     window.scrollTo、脚本运行结束都会发 → 嵌入者把
  │     live offset 与滚动条 snap 到它
```

`WidgetWebview::onRepaint` 里的位移公式（`WidgetWebview.cc`）：

```cpp
int dx = r.x + (m_frameScrollX - m_scrollX);
int dy = r.y + (m_frameScrollY - m_scrollY);
```

注意 **`EWEB_MOUSE_WHEEL` 引擎直接忽略**——滚轮手势由嵌入者自己换算成 `ewebview_scroll()`（文档坐标由引擎换算，见 7.3 的命中测试）。

## 11.5 监听器钩子逐个看

| 钩子 | 语义与典型用法 |
| --- | --- |
| `on_frame` | 见 11.3。唯一不可省略的钩子 |
| `on_scroll` | 引擎权威滚动偏移（换页归零 / `window.scrollTo` / 脚本运行结束）。snap live offset 与滚动条 |
| `on_url` | 可见页 URL 变了（地址栏加载或页内导航）。**嵌入者拥有会话历史**——在这里记录 |
| `on_title` | `<title>` 或 `document.title`，刷窗口标题 |
| `on_status` | 状态栏文本 + 进度（链接目标、加载消息、`alert()` 文本） |
| `on_build_status` | 构建遮罩：`overlay=true` 仅在**真实构建**进行中（post-swap 脚本运行不算）——可以罩住页面显示 `text` + 0..100 进度条 |
| `on_cursor` | 悬停链接时请求光标形状（如 `"pointer"`）；可能为 NULL/`"default"` |
| `on_dialog` | `alert/confirm/prompt` 文本，**非阻塞**呈现（引擎永不等待模态；confirm 按 cancel、prompt 按 null 应答） |
| `on_task_start/end/failed` | 子资源任务生命周期（类型为 `EWEB_TASK_HTML/CSS/IMAGE`），驱动进度指示 |
| `on_tasks_end` | 任务队列排空——熄灭加载指示的时机 |

## 11.6 输入映射

```c
typedef struct eweb_event {
    int mouse_state;  /* EWEB_MOUSE_MOVE/DOWN/UP/CLICK/DOUBLE_CLICK/WHEEL */
    int button;       /* EWEB_BUTTON_NONE/LEFT/MIDDLE/RIGHT */
    int cx, cy;       /* 视口左上原点 client 坐标 */
    int wheel;        /* 仅 WHEEL：-1 上 / +1 下（行） */
} eweb_event_t;
```

`cx/cy` 必须由嵌入者算好（引擎不知道嵌入者的窗口几何）。引擎侧负责：DOM 鼠标事件派发（捕获→目标→冒泡，见 7.3）、hover 跟踪、`<a href>` 点击跟随（8px 抖动门限）。

## 11.7 配置面

- `ewebview_set_viewport(w, h)`：重排 + 重绘 + 派发 `window.onresize`；
- `ewebview_set_default_css(url)`：换 UA 默认样式表（首个页面加载前调，见 5.4）；
- `ewebview_set_js_enabled(bool)`：默认开。关闭会释放 VM，下一次加载剥掉 `<script>`；
- `ewebview_stop()`：中止在途加载，保留屏幕上已有内容；`ewebview_reload()`：重载当前页；`ewebview_get_url()`：当前可见页 URL（引擎持有，下次导航前有效）。

## 11.8 参考实例：WidgetWebview 与 xBrowser

**WidgetWebview**（`browser/libs/widget++/src/WidgetWebview/`）是标准嵌入范式，映射关系一目了然：

| widget++ 虚函数 | ewebview 调用 |
| --- | --- |
| 构造/析构 | `eweb_port_ewokos` + `ewebview_create` / 归还收养帧 + `ewebview_destroy` |
| `onTimer` | `ewebview_tick()`（30Hz 窗口定时器驱动） |
| `onRepaint` | blit 收养的帧（`frameGraph` 零拷贝强转 + 11.4 位移公式） |
| `onResize` | `ewebview_set_viewport()` |
| `onMouseEvent` | 换算 client 坐标 → `ewebview_post_event()` |
| `onScroll` | `uiLocalScroll()`：clamp → 移动 live offset → `ewebview_scroll()` |

**xBrowser**（`browser/apps/xBrowser/`）在 WidgetWebview 之上加应用逻辑：`BrowserWidget` 覆写 `onTaskFailed`——`TASK_HTML` 失败且重试未满 6 次时记下 URL、按 30Hz 定时器约 1.5 秒后自动 `reload()`；`onTasksEnd` 归零重试计数。状态栏、标题栏、地址栏分别消费 `on_status/on_title/on_url`。

链接顺序（`apps/xBrowser/Makefile`，静态库按依赖序）：

```
$(EWOK_LIB_X) -lWidgetWebview -lewebview -lmario -lwebp -llitehtml \
              -ltinyhttpsc -lsocket $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
```

`libewebview.a` 在 WidgetWebview 之后；`eweb_port_ewokos()` 被 WidgetWebview 引用，因此参考移植对象文件随库被拉入（见 10.6 的按需链接）。

## 11.9 嵌入检查清单

- [ ] 每个 UI tick 都调 `ewebview_tick()`（回调全部在这里发生）
- [ ] `on_frame` 收养的每帧最终都经 `ewebview_release_frame()` 归还；`ewebview_destroy()` 之前全部还清
- [ ] 监听器回调不阻塞、不长时间占用 UI 线程
- [ ] 滚动手势走"本地位移 + `ewebview_scroll()`"双步，不要只调 API 等帧
- [ ] 会话历史（前进/后退栈）记在 `on_url` 里——引擎不管历史
- [ ] 窗口尺寸变化即 `ewebview_set_viewport()`，否则排版停在旧视口
- [ ] 用 `on_task_*`/`on_build_status` 给出加载反馈；用 `onTaskFailed(TASK_HTML)` 做重试
