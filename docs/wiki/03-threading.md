# 第 3 章 · 线程模型与帧交付

ewebview 的并发设计回答了三个问题：UI 如何永不阻塞、两条后台线程如何分工、渲染结果如何零拷贝地跨线程上屏。所有规则都写在 `ewebview.h` 与 `ewebview_port.h` 的注释契约里，本章展开实现侧。

## 3.1 三类线程

```
┌──────────────────────┐
│ UI 线程（嵌入者拥有）   │  ewebview_load/stop/scroll/post_event  → 投命令
│                      │  ewebview_tick()                         → 收事件、发监听器回调
│                      │  ewebview_release_frame()                → 归还帧
└────────┬─────────────┘
         │ m_cmdQueue（pthread_mutex+cond）
         ▼
┌──────────────────────┐
│ 引擎线程（每个实例 1 条）│  独占：litehtml 文档×2、容器×2、mario VM、
│  engineLoop()        │  正在绘制的表面、画布注册表
└────────┬─────────────┘
         │ m_taskQueue / m_resultQueue（各一把 mutex）
         ▼
┌──────────────────────┐
│ 下载线程（按需 1 条）    │  net.request / net.read_file / image.decode
│  taskLoop()          │  只碰任务与结果队列，做完即退出
└──────────────────────┘
```

- **引擎线程**在 `ewebview_create()` 时即创建（`engineStart()`），创建后停在 `m_cmdCond` 上待命，直到第一个命令到来。它**独占**一切非线程安全对象：两份 litehtml 文档（可见页 + 构建中页）、两个 litehtml context、mario VM、它正在绘制的表面。**它从不触碰嵌入者的窗口。**
- **下载线程**按需拉起（`addTask()` 发现没有活着的 worker 就 `pthread_create` 一条 `taskLoop()`），队列取空后自行退出，下次有任务再起一条。
- **UI 线程**是嵌入者自己的主循环。它与引擎之间只有两条通道：命令队列（UI→引擎）与事件队列（引擎→UI），外加帧池指针的所有权交接。

## 3.2 UI → 引擎：命令队列

公开 API 的每个动作都只是**排队一条命令后立即返回**（`ewebview.h` 注释：窗口永不因下载/解析而阻塞）：

| 公开 API | 命令 | 载荷 |
| --- | --- | --- |
| `ewebview_load(url)` | `ECMD_NAVIGATE` | url |
| `ewebview_stop()` | `ECMD_STOP` | — |
| `ewebview_reload()` | `ECMD_RELOAD` | — |
| `ewebview_set_viewport(w,h)` | `ECMD_RESIZE` | w,h |
| `ewebview_scroll(x,y)` | `ECMD_SCROLL` | x,y |
| `ewebview_post_event(&ev)` | `ECMD_INPUT` | `eweb_event_t` 拷贝 |
| `ewebview_set_js_enabled(b)` | `ECMD_SET_JS` | b |
| （析构内部） | `ECMD_SHUTDOWN` | 终止引擎循环 |

四个**终止触发器**（STOP / NAVIGATE / RELOAD / SHUTDOWN）都在引擎线程上执行清理（`engineHandleCommand`）——文档、VM、表面由拥有它们的线程亲自销毁，不存在跨线程 delete。

`postCommand()` 可被任何线程调用：加 `m_cmdMutex` 入队，`pthread_cond_signal(&m_cmdCond)` 唤醒引擎。

## 3.3 引擎 → UI：事件队列与 tick()

引擎产出 `EWebUiEvent`（`EWebInternal.h`）：

| 事件 | 对应监听器钩子 | 含义 |
| --- | --- | --- |
| `EUET_FRAME` | `on_frame(frame, sx, sy, doc_w, doc_h)` | 一帧渲染完毕，**所有权移交** |
| `EUET_SCROLL_CLAMP` | `on_scroll(x, y, doc_w, doc_h)` | 引擎权威滚动值（换页归零、scrollTo 等） |
| `EUET_URL` | `on_url(url)` | 可见页 URL 变化（含页内导航） |
| `EUET_BUILD_STATUS` | `on_status` / `on_build_status(text, progress, overlay)` | 构建进度与遮罩 |
| `EUET_TITLE` | `on_title(title)` | `<title>` 或 `document.title` |
| `EUET_DIALOG` | `on_dialog(text)` | alert/confirm/prompt 文本（非阻塞） |
| `EUET_TASK_START/END/FAILED/TASKS_END` | `on_task_*` | 子资源任务生命周期（进度条/转圈） |

`ewebview_tick()` 是**唯一**会回调嵌入者的地方，保证所有监听器都在 UI 线程上跑。它的实现有两个关键细节（`ewebview.cc` `EWebEngine::tick()`）：

1. **先整体搬出再回调**：在 `m_uiMutex` 下把队列整个拷贝出来、快照监听器表，然后在**锁外**逐个触发回调——嵌入者的回调可以自由地再调引擎 API（比如 `on_url` 里又 `load()`），不会死锁。
2. **监听器中途切换安全**：`ewebview_set_listener()` 同样在 `m_uiMutex` 下按值拷贝，本拍内已快照的继续用旧表，下一拍生效。

嵌入者的义务：**每个 UI 节拍调一次 `ewebview_tick()`**（WidgetWebview 在 `onTimer` 里调）。

## 3.4 帧池：所有权转移 + 背压

帧交付是这套设计里最精巧的部分，目标是**跨线程零像素拷贝**：

```
引擎                          UI
 │  engineEnsureFramePool()    │
 │  从 m_freeFrames 取一块表面   │
 │  光栅化整个视口进去           │
 │  m_pendingFrame = buf       │
 │  发 EUET_FRAME ───────────► │ tick(): frame = m_pendingFrame
 │                             │        m_pendingFrame = nullptr
 │                             │ on_frame(frame) → 嵌入者【认领】
 │                             │   （当作显示缓存，repaint 时 blit）
 │                             │ 用完后 ewebview_release_frame()
 │  ◄──────────── 归还 m_freeFrames
```

规则：

- **所有权随 `on_frame` 移交**。嵌入者通常把它存为显示缓存，在**认领下一帧之前**把上一帧 `ewebview_release_frame()` 还回池中。
- **未归还前引擎不会碰这块表面**——天然背压：如果 UI 还没来得及认领上一帧（`m_pendingFrame != nullptr`），`engineRenderFrame()` 直接跳过本次渲染，帧池永不膨胀，也没有逐帧分配。
- 帧事件附带**渲染时的滚动偏移** `(frame_scroll_x, frame_scroll_y)` 与文档全尺寸 `(doc_w, doc_h)`。快速滚动时 UI 的活滚动值可能领先渲染值，嵌入者把帧按 `frameScroll - liveScroll` 平移 blit 即可避免撕裂感（见第 11 章）。
- 归还帧不检查尺寸：`gfx` 表按契约是引擎线程专用，尺寸校验在 `engineEnsureFramePool()` 里于引擎线程完成（resize 后旧尺寸的表面被丢弃重建）。
- `tick()` 与 `releaseFrame()` 在处理完帧后都会 `pthread_cond_signal(&m_cmdCond)`，把引擎从"等认领"的停车状态立即唤醒去画下一帧。

## 3.5 引擎主循环（engineLoop）

`ewebview.cc` 的 `EWebEngine::engineLoop()` 每轮做十件事，顺序即优先级：

```
1. 排空命令队列（SHUTDOWN 则退出循环）
2. 落地下次交换遗留的延迟删除（旧文档/旧容器）
3. 排空下载结果队列 processResults()
4. 处理可见页与构建页的样式/布局脏标记 applyPendingLayoutUpdates()
5. build 状态机推进一步 advanceBuildStep()
6. 执行脚本请求的导航/滚动 jsRunPendingNavigation()
7. 轮询 JS 定时器 jsPollTimers()（有回调触发则标脏）
8. 挂载构建期延迟加载的图片 flushPendingImages()
9. 页面变化或滚动偏移变了 → engineRenderFrame()
10. 无事可做则限时停车（见下）
```

**停车策略**（空闲零 CPU）：

| 状态 | 超时 |
| --- | --- |
| 有帧等 UI 认领 | 16 ms |
| 构建中/有脏标记/待处理工作 | 4 ms（让步但近乎实转） |
| 页面空闲但 VM 活着 | 16 ms（定时器粒度） |
| 完全空闲 | 200 ms |

等待一律是**有上限的** `pthread_cond_timedwait`——信号丢失最多晚一个超时，不会睡死；而 `postCommand()` / `pushResult()` / `releaseFrame()` 都会 signal，真实事件即时唤醒。锁序约定：`m_resultMutex` 与 `m_cmdMutex` 永不嵌套，不会与 worker/UI 死锁。

循环退出（收到 SHUTDOWN）后，`engineTeardown()` **在引擎线程自己的上下文里**释放文档、VM、画布与帧池，然后线程返回——析构函数此时 `pthread_join`，再销毁 mutex。这就是"`ewebview_destroy()` 安全"的全部依据。

## 3.6 移植层的线程契约

`ewebview_port.h` 给每张表标注了它会被哪条线程调用，移植时必须遵守：

| 表 | 调用线程 | 要求 |
| --- | --- | --- |
| `gfx.*` | 仅引擎线程 | 无需内部锁 |
| `font.*` | 仅引擎线程 | 无需内部锁 |
| `image.decode` | 仅下载线程 | 纯堆操作，不得碰引擎状态 |
| `net.*` | 仅下载线程 | 每次 request 独立，可重入 |
| `clock.tic_ms` | 引擎 + 下载线程 | 可重入 |
| `sys.log` | 引擎线程 | — |
| `sys.ptr_sane` | 引擎线程 | 廉价、不解引用 |

EwokOS 参考 port 里用到的原语（graph/font/tinyhttpsc/kernel_tic）都不保存跨调用状态，天然满足上述约束。

## 3.7 为什么不需要更多锁

- litehtml 文档、mario VM、画布：单线程（引擎）所有权，零锁；
- CookieJar：被下载线程（存 Set-Cookie）与引擎线程（document.cookie）共享，**自己带一把 mutex**（`EWebCookies.h`）；
- 任务/结果队列：各一把 mutex，worker 与引擎无嵌套取锁；
- 帧池：只交接指针，`m_uiMutex` 一把窄锁；
- 监听器表：UI 线程读写 + `m_uiMutex` 保护引擎侧投事件时的快照。

整体锁数量极小，且全部单向、无锁序环——这是"引擎独占 + 队列通信"模型带来的直接收益。
