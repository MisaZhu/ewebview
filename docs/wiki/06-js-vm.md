# 第 6 章 · JavaScript：mario VM 集成与脚本调度

ewebview 的 JS 由 **mario VM**（`mario_js/` 子模块，纯 C 字节码虚拟机 + JS 前端）执行。litehtml 本身没有脚本能力，所以引擎在 HTML 进解析器之前先把 `<script>` 剥出来，再按页面生命周期分阶段喂给 VM。本章讲 VM 的生命周期、脚本调度时机、看门狗与 document.write 重解析。

mario VM 自身的内部设计（字节码、编译器、GC）有独立文档：[mario_js/docs/wiki/](../../mario_js/docs/wiki/README.md)，本章只讲"引擎怎么用它"。

## 6.1 脚本抽取：解析前剥离 `<script>`

`ewebview.cc` 的 `extract_scripts(html, &scripts, &has_inline_handlers)`：

- 扫描全文，把每个 `<script>...</script>` 块从 HTML 里**删掉**，内联脚本体按文档序追加到 `scripts` 列表；
- `<script src="...">` **外链脚本直接丢弃**（当前不支持外部脚本的下载执行）；
- `type` 存在且不含 `javascript`（如 `application/json`、`module`）的块跳过，不执行；
- 标签匹配大小写不敏感；
- 同时报告页面是否带**内联事件属性**（`onclick="..."` 等）——即使没有 `<script>` 块，带内联事件的页面也需要 VM。

为什么解析前剥离：litehtml 不认识 JS，`el_script` 会留下一段什么都不渲染的文本；而脚本的执行时机（文档建成后）与解析时机必须分开。

## 6.2 VM 生命周期

**创建（`initJsVm()`，惰性）**——页面同时满足"JS 开启 + 有脚本或有内联事件"才建 VM。纯标记页面永远不付 VM 的内存：

```
vm_new(js_compile, ...)          // 编译器函数指针挂进 VM
vm_init(vm, reg_all_natives, ..) // 装内建类：Object/Array/String/Console/...
vm->step_interval = 32768        // 每 32768 条指令回调一次 step 钩子
vm->on_step       = jsVmStepHook // 看门狗 + 中止检查（见 6.4）
js_register_dom_natives(vm, this, &cb)   // 四套桥按序注册（第 7 章）
registerCanvasNatives(vm)
registerEventNatives(vm)
registerWebNatives(vm)
```

**mario 的平台钩子**是三个进程级函数指针：`_platform_malloc` / `_platform_free` / `_platform_out`。引擎把它们接到 libc malloc/free 与 port 的 `sys.log`（JS `console.log` 由此以 `[js]` 前缀输出）。因为 `_platform_out` 没有 ud 参数，引擎用一个文件级静态指针指向"最后跑过 VM 的那个引擎"的日志钩子——单浏览器场景下这是正确的作用域。

**销毁**：`vm_close()` 释放 VM 及其一切对象。每次导航（换页）都会重建 VM——页面之间的 JS 全局态**不**延续（延续的是 Cookie 与 localStorage，见第 9 章）。

## 6.3 脚本调度的时机与次序

```
CREATE_DOC ──► (可能 document.write?) ──是──► RUN_JS（先跑，见第 4 章 4.5A）
                    │否                            │ document.write 输出拼回 HTML
                    ▼                              ▼ 重回 CREATE_DOC（上限 8 次）
              RENDER_DOC → SWAP_DOC（先上屏）
                    │
                    ▼ RUN_JS（渐进，4.5B）：每轮引擎循环跑一个脚本
              全部跑完 → jsFireLoadEvents() → IDLE
```

- **多脚本共享全局**：`vm_load_run()` 把当前脚本的字节码接在前一个之后执行，全局量存于 `vm->root`——与浏览器里多个 `<script>` 块共享一个全局作用域一致；
- **渐进可见**：post-swap 模式下脚本的 DOM 修改经 `jsProgressiveFlush()` 中途重排重绘（间隔自适应：上次 flush 耗时 ×3，夹在 200~1500ms）；
- **load 事件最后发**：`DOMContentLoaded` → document/window `load` → `<body onload>`，保证监听器看到的是布局完毕、已上屏的文档；
- **存活期**：之后脚本的入口只剩两类——定时器（引擎循环第 7 步 `jsPollTimers()` → `js_dom_poll_timers(vm, ticMs())`）与输入事件（`jsDispatchMouseEvent()`，第 7 章）。

脚本**操作哪份文档**由 `jsActiveDoc()` 决定：构建期是 `m_buildDoc`，上屏后是 `m_doc`。`BUILD_SWAP_DOC` 把同一个 document 对象移交，所以脚本持有的元素句柄跨交换不失效。

## 6.4 运行预算看门狗

脚本死循环不能锁死浏览器。三级防护：

1. **step 钩子**：VM 每执行 32768 条指令回调 `jsOnVmStep()`；
2. **运行预算**：单次 VM 运行（一段页面脚本、一次定时器回调、一个事件处理）超过 `kJsRunBudgetMs = 10000ms` → 置 `vm->terminated = true`，VM 全线退绕；
3. **计数淘汰**：`jsVmExit()` 发现这次运行是被看门狗终止的，`m_jsAbortCount++`；累计 `kJsRunAbortMax = 3` 次 → `m_jsPageDisabled = true`，**本页 JS 被整体关停**直到下次导航。

**代计数防误伤**：`jsVmEnter()` 快照 `m_buildAbortGen`。如果一次运行的终止是因为用户点了停止/后退（代计数在运行期间变了），这次终止**不计**违规——只有货真价实的超时才计。`jsVmExit()` 还会做 `vm_terminate()` 清理退绕留下的操作数/作用域残帧，让 VM 能服务下一个回调。

## 6.5 document.write：重解析与状态存活

`document.write()` 在现代引擎里是解析器插入点语义；ewebview 用**整页重解析**近似：

1. js_dom 桥把脚本的 write 内容累积进缓冲区，`js_dom_take_write_buffer()` 排出；
2. `applyJsWriteBuffer()` 把内容**插到 `</body>` 之前**（没有则追加到尾部）；
3. 重解析上限 `kJsMaxReparse = 8`；重建时**清空脚本列表**防止死循环；
4. 重建文档前 `jsInvalidateHandles()`：`js_event_clear_listeners` + `js_dom_reset_element_cache`——旧树死了，句柄缓存与监听器不能留；
5. VM 本身**不重建**——它要重跑拼回页面的全部脚本，全局从头再来；
6. **mutation 日志**（`m_jsMutations`）：脚本对 DOM 的写（文本/属性/标题三类）都记一条 `(kind, id, name, value)`，同 key 新值覆盖旧值；新文档建成时 `replayJsMutations()` 按元素 id 重放——于是 `document.getElementById('x').textContent = ...` 之后接 `document.write` 的页面，重解析后改动还在。

## 6.6 元素句柄的活性管理

JS 侧的元素句柄是裸 `litehtml::element*`。脚本可能比节点活得久（节点被 `innerHTML` 赋值或 `removeChild` 摘掉了），防线有两道：

- **`el_is_live` 回调**（js_dom 桥）：每次从 JS 包装还原句柄前检查活性标记；
- **`sys.ptr_sane`**（可选移植钩子，EwokOS 是 `ewok_ptr_in_heap`）：在读活性标记之前先做一次**不解引用**的堆内指针合理性检查——挡掉伪造句柄。

被 `removeChild`/`innerHTML` 摘下的子树不立即删除，而是**泊进 `m_jsDetached`**（脚本可能还持有指向它们的句柄或监听器），页面销毁时由 `jsFreeDetachedNodes()` 统一释放；若脚本把节点重新插回树（`appendChild`），它会先从停泊列表摘除（`js_unpark`）。

`innerHTML` 的 setter 用**标签剥离**近似（litehtml 没有 HTML 片段解析器）：只保留文本内容，替换为单个 `el_text` 子节点。

## 6.7 JS 侧能力速查

- **语言**：ES5 全部 + ES6+ 大量特性（class、箭头函数、let/const、模板串、Promise、Proxy、TypedArray、BigInt、RegExp……详见 mario_js 文档第 11 章）；
- **DOM**：getElementById/querySelector(All)/getElementsBy*、createElement/TextNode、树遍历与增删、textContent/innerText/innerHTML、属性与 classList、style 读写、offsetX/clientX/getBoundingClientRect、focus/scrollIntoView；
- **Event**：完整捕获→目标→冒泡传播、`addEventListener`（capture/once）、`dispatchEvent`、on* 属性、内联 on\* 属性按需编译执行；
- **BOM**：location/history/navigator/screen/performance、localStorage/sessionStorage、document.cookie、同步 XHR 与 fetch、atob/btoa、crypto.getRandomValues；
- **Canvas**：完整 CanvasRenderingContext2D + Path2D + ImageData（第 8 章）。

已知留白（js_web.h 头部"Known gaps"）：`postMessage` 空操作；`globalThis` 别名 `window` 而非 VM 全局；`performance.mark/measure` 不存时间线；`crypto.getRandomValues` 用单调时钟播种（只能做 id）；XHR 永远同步。
