# 第 4 章 · 页面加载流水线（build 状态机）

从 `ewebview_load(url)` 到新页上屏，中间隔着下载、解析、样式、脚本、布局多个阶段。ewebview 把它们组织成一台**非阻塞状态机**：`m_buildPhase` 驱动，引擎主循环每轮推进一步（`advanceBuildStep()`），期间 UI 随时可中止。本章逐阶段拆解。

## 4.1 状态总览

```c
enum BuildPhase {
    BUILD_IDLE = 0,        // 无构建：当前页存活
    BUILD_PRELOAD_CSS,     // 等 UA 默认样式表（master.css）就绪
    BUILD_CREATE_DOC,      // gumbo+litehtml 解析 HTML，建树
    BUILD_RUN_JS,          // 跑脚本（两种模式，见 4.5）
    BUILD_RENDER_DOC,      // 全量样式 + 布局
    BUILD_SWAP_DOC,        // 新页替换旧页上屏
    BUILD_FAILED,          // 失败收尾
};
```

正常路径：

```
engineNavigate() 排队 HTML 任务
   → (HTML 到手) PRELOAD_CSS → CREATE_DOC → [RUN_JS] → RENDER_DOC → SWAP_DOC
   → [RUN_JS 渐进跑余下脚本] → IDLE
```

构建期间旧页**始终留在屏幕上**，由 `on_build_status(text, progress, overlay=true)` 通知嵌入者盖一层进度遮罩；只有 `BUILD_SWAP_DOC` 瞬间新旧页交接。

## 4.2 起点：engineNavigate()

`ECMD_NAVIGATE` / `ECMD_RELOAD` / 脚本导航最终都进 `engineNavigate()`（引擎线程）：

1. `engineDropPendingWork()`——丢弃所有排队/已完成的下载（旧页资源不再需要）；
2. `cleanupBuildResources()`——终止上一个未完成的构建（释放 build 文档/容器、重置 VM、清画布）；
3. 滚动归零、帧缓存失效；
4. **轮换 litehtml context**：`m_browser_context` 与 `m_buildContext` 双缓冲交替，新页的 master 样式集从零开始，而不惊动可见页正在用的那份；
5. 记录 SameSite 发起方（`setTaskPageUrl(m_currentHtmlUrl)`，见第 9 章）；
6. 排队 `EWEB_TASK_HTML` 任务。

注意 **可见页 `m_doc` 此刻不释放**——它继续上屏，直到 SWAP 时被替换并延迟删除。

## 4.3 PRELOAD_CSS → CREATE_DOC

**BUILD_PRELOAD_CSS**（进度 15%）：等 UA 默认样式表。`ewebview_set_default_css()` 指定的 master.css 在第一个页面之前加载；条件满足即进入下一阶段。

**BUILD_CREATE_DOC**（进度 45%）：

- 新建 `EWebContainer`（绑定 port 与引擎 host），设置视口尺寸与页面 base URL（相对链接解析的基准）；
- `setDeferImageLoad(true)`：构建期图片只记 URL 不发任务（避免为半成品页浪费下载）；
- `litehtml::document::createFromString(html, container, ctx)`——**整个流程中最长的单次调用**（gumbo 解析 + 全 DOM 建树）。它在引擎线程跑，冻不了 UI，但必须**可中止**：容器的 `create_element`/`text_width` 等热点回调会轮询 `buildAbortRequested()`（见 4.7），一旦置位就抄短路返回，让解析快速退绕；
- 解析完 `replayJsMutations()`：如果这是 document.write 触发的重解析，把上轮脚本改过的 text/attr/title 重放到新树上（见第 6 章）；
- 有 `document.write` 嫌疑（`m_jsRunBeforePaint`）且有脚本 → `BUILD_RUN_JS`（先跑脚本模式）；否则直接 `BUILD_RENDER_DOC`。

## 4.4 RENDER_DOC：全量样式 + 布局

`BUILD_RENDER_DOC`（进度 80%）做两件事：

1. **全量样式计算**：文档在"快模式"（fast mode）下创建——`parse_styles` 跳过整个盒模型，`100vh`、`attr(width)` 这类依赖会塌成 0。所以布局前先关快模式，强制一轮完整样式遍历。遍历是**分片**的：`update_master_styles_step(deadline)` 每片最多 `kStyleBudgetIdleMs = 25ms`，片间引擎回主循环排命令/画帧，中止标志可以在遍历中途生效。
2. **布局**：`m_buildDoc->render(m_clientWidth)` 计算全部几何。随后打印性能统计（`text_width` 调用/耗时/缓存命中率等，`EWEBVIEW_DEBUG` 下可见）。

完成后进入 `BUILD_SWAP_DOC`。

## 4.5 RUN_JS 的两种模式

脚本执行时机由页面是否可能 `document.write` 决定：

**A. 先跑脚本（pre-paint，`m_jsPostSwapRun == false`）**
仅用于可能写文档的页面。在建好的 DOM 上跑完全部内联脚本 → `applyJsWriteBuffer()` 把脚本的 `document.write()` 输出**拼回 HTML 源码**（插在 `</body>` 前）→ 状态机退回 `BUILD_CREATE_DOC` 重新解析。重解析有上限 `kJsMaxReparse = 8`，且重建时脚本列表清空，防止死循环。

**B. 渐进跑脚本（post-swap）**
普通页面**先上屏再跑脚本**：SWAP 之后每轮引擎循环跑一个脚本（`runNextPageScript()`），脚本里的 DOM 修改经 `jsMarkLayoutDirty() → jsProgressiveFlush()` 在**脚本运行中途**就把阶段性结果绘制出去——测试页的结果是一条条冒出来的，而不是白屏等到底。渐进重排的间隔自适应：上一次的 flush 成本 ×3，夹在 `kJsFlushMinGapMs = 200` 与 `kJsFlushMaxGapMs = 1500` 之间。

两种模式收口处都会 `jsFireLoadEvents()`：先 `DOMContentLoaded`，再 document/window 的 `load`，再 `<body onload>`——**只在页面脚本跑完之后**，与每个页面假设的顺序一致。

## 4.6 SWAP_DOC：新旧页交接

`BUILD_SWAP_DOC`（进度 100%）是流水线里唯一的"指针搬家"：

```c
m_doc       = m_buildDoc;        // 新文档转正（同一个对象，JS 元素句柄不失效）
m_container = m_buildContainer;
m_activeContext = m_buildTargetContext;
m_pendingDeleteDoc       = old_doc;        // 旧页延迟到下一轮循环删除
m_pendingDeleteContainer = old_container;  // （此刻删会重进仍在用的容器）
m_engineScrollX = m_engineScrollY = 0;     // 新页回到顶部
m_cacheValid = false;                      // 帧缓存作废 → 立即重绘
m_flushDeferredImages = true;              // 构建期压下的图片现在发任务
```

随后 `postScrollClamp()` 发 `EUET_SCROLL_CLAMP` 让 UI 同步滚动条。若还有脚本没跑（渐进模式），状态机留在 `BUILD_RUN_JS` 继续；否则回 `BUILD_IDLE`，页面进入存活期。

`BUILD_FAILED`：内容为空或解析失败时进入，下一轮 `cleanupBuildResources()` 收尾。

## 4.7 中止：让半成品页快速死亡

用户在下载/解析/排版/跑脚本的任何时刻都可能点停止、后退或输入新 URL。中止链路：

1. UI 线程 `ewebview_stop()` 等 → 命令入队；`requestBuildAbort()` 把 `m_buildAbort`（volatile）与 **`m_buildAbortGen` 代计数**置起；
2. 正在解析的 litehtml 通过容器热点回调里的 `buildAbortRequested()` 短路退出；
3. 正在跑脚本的 mario VM 被 step 钩子终止（`jsOnVmStep` 比对代计数，见第 6 章）；
4. 下一轮 `advanceBuildStep()` 开头看到 `m_buildAbort` → `cleanupBuildResources()` 丢弃半成品（但**保留**刚排队的待导航 URL，那是用户真正想去的页）。

**代计数（generation）是精度关键**：它能区分"本次运行期间刚到的中止"（立即退绕，不算脚本违规）与"空闲时消化掉的陈旧 STOP"（不影响当前页脚本的看门狗计数）。

## 4.8 布局脏标记与防抖

页面存活期的重排不立即发生，走防抖合并（`EWebInternal.h` 常量）：

| 常量 | 值 | 作用 |
| --- | --- | --- |
| `kLayoutDebounceMs` | 30 ms | 标脏后至少等这么久，合并连续变更 |
| `kLayoutMaxWaitMs` | 200 ms | 脏标记最长挂起时间，到点强制重排 |
| `kLayoutBehindStyleMs` | 500 ms | 样式积压超过它说明落后太多，走补偿路径 |
| `kStyleBudgetIdleMs` | 25 ms | 单次样式分片的墙钟预算 |
| `kJsMaxReparse` | 8 | document.write 重解析上限 |
| `kJsRunBudgetMs` | 10000 ms | 单次 VM 运行预算（第 6 章） |

`applyPendingLayoutUpdates()`（引擎循环第 4 步）按这些水位线决定本轮是否真的执行样式/布局，结果脏了才设 `m_contentDirty` 触发重绘。渲染因此**只在必要时发生**，而每次渲染只画不重排。

## 4.9 进度与状态汇报

| 阶段 | 文本 | progress | overlay |
| --- | --- | --- | --- |
| PRELOAD_CSS | "loading styles" | 15 | 1 |
| CREATE_DOC | "building document" | 45 | 1 |
| RUN_JS（先跑） | "running scripts" | 60 | 1 |
| RENDER_DOC | "layout and first paint" | 80 | 1 |
| SWAP_DOC | "displaying page" | 100 | 1 |
| SWAP 后的渐进脚本 | "running scripts" | 100 | **0**（页已上屏，只亮状态栏） |
| 完成/中止 | "" | 0 | 0 |

嵌入者据此画进度条与遮罩；`overlay` 为真才遮页面（`on_build_status`）。
