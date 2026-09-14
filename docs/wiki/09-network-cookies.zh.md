# 第 9 章 · 网络、Cookie 与存储

> 语言: [English](09-network-cookies.md) | **中文**

ewebview 的网络子系统解决三个问题：子资源怎么在**不阻塞引擎线程**的前提下进来；重定向与 Cookie 怎么在**跨站**时仍然安全；JS 的 `document.cookie` 与 `localStorage` 怎么跟 HTTP 侧共享状态。核心结构只有两条队列、一个下载**线程池**和一个进程级 CookieJar。

## 9.1 子资源任务队列与下载线程池

```
引擎线程（生产者）                     下载线程池（消费者，0..8 个按需拉起）
  addTask({url,type}) ──► m_taskQueue ──► getTaskLocked() 取一个未 loading 的
        ▲                                    │ loadHtmlTask / loadCSSTask / loadImageTask / loadScriptTask
        │                                    │   └─ EWebContainer::loadURL()（可能含重定向）
 m_resultQueue ◄── pushResult({url,ok,content,image})
        │
        ▼ 引擎循环第 2/3 步 processResults()（见第 3 章）
   HTML→启动 build 状态机 / CSS→loadCSSContent / 图片→mountDecodedImage / SCRIPT→填回有序脚本槽
```

- **任务类型**即公共常量 `EWEB_TASK_HTML / EWEB_TASK_CSS / EWEB_TASK_IMAGE / EWEB_TASK_SCRIPT`（嵌入者经 `on_task_*` 监听器看到的就是它们；`EWEB_TASK_SCRIPT` 是外链 `<script src>` 的下载，见第 6 章）。
- **`addTask` 按 URL 去重**（同一图片在页面上出现十次只下载一次）。线程池**默认为空**（`m_taskThreads == 0`）；每次 `addTask` 先**唤醒一个停放的 worker**（`pthread_cond_signal(m_taskCond)`），只有当所有存活 worker 都在忙（`m_taskThreads - m_taskBusy <= 0`）时才 `pthread_create` + `pthread_detach` **扩容线程池**，上限 `kTaskPoolMax = 8`。无事可做的 worker 在 `m_taskCond` 上分片停放，**空闲超过 `kTaskIdleExitMs = 4000` ms 就自行销毁**，于是一页的抓取排空后线程池回缩到零；队列排空且无 worker 在抓取中（`m_taskBusy == 0`）时发一次 `EUET_TASKS_END`。
- **worker 的纪律**：只碰互斥锁保护的 `m_taskQueue`/`m_resultQueue` 和移植表的 `net.*`/`image.decode` 回调，**永远不碰文档与 VM**；对 UI 的通知（`EUET_TASK_START/END/FAILED`）一律 `postUiEvent`，由 UI 线程在 `ewebview_tick()` 里投递——worker 从不直接调嵌入者钩子。由于最多 8 个 worker 并发运行，它们共享的一切（两条队列、`m_taskPageUrl`、CookieJar，以及移植层的 `net.request`/`image.decode`）要么受互斥锁保护，要么逐次调用可重入。
- **topLevel 语义**：`loadHtmlTask` 传 `topLevel=true`（顶层导航），CSS/图片传 `false`（子资源）。这个布尔与 `pageUrl`（发起文档的 URL）一起驱动 SameSite 判定（见 9.4）。
- **图片在 worker 线程解码**：`loadImageTask` 下载完就地 `decodeImageData()`（纯堆操作），引擎线程收到结果时位图已就绪，只做 O(1) 的 `mountImage` 挂载——单张图几十毫秒的解码不会卡住排版。

## 9.2 loadURL：重定向跟随与逐跳 Cookie 作用域

所有入口（HTML/CSS/图片，以及 `file://`）都汇到 `EWebContainer::loadURL()`。`file://` 走 `net.read_file`；`http(s)://` 走 `net.request`/`net.free_response`，并且**重定向在引擎侧跟随**（上限 `kMaxRedirects = 5` 跳），而不是让 HTTP 库内部跟随。三个理由：

1. **Cookie 跨站泄漏**：库内跟随会把第一跳的 `Cookie:` 头原样带给重定向后的每一个主机。引擎侧跟随则**每跳重新计算** Cookie 头——`EWebCookieJar::requestHeader(cur_url, pageUrl, topLevel)` 只放行本跳的 domain/path/Secure/SameSite 规则允许的条目，跳到外站时旧站的 Cookie 自然留在家中。
2. **Set-Cookie 丢失**：库内跟随只保留最后一跳的响应头。引擎侧在**任何处理之前**先把本跳的全部 `Set-Cookie` 存进 Jar——登录流程恰恰是在 302 上设会话 Cookie 的，错误状态响应也可能带。
3. **相对 Location**：`Location: /login` 需要对"发起这一跳的 URL"解析，库内跟随拿得到最终 URL 却丢失了中间上下文。

落地规则：请求失败、`resp.error`、非 200 状态、空 body 一律返回 NULL（失败结果同样进 `m_resultQueue`，由引擎按类型降级——CSS 失败就 `forgetCSS`，图片失败就保持 0×0 布局）。

**移植层配合**：两份参考移植（`port_ewokos.c` 与 `port_sdl2.c`）都显式 `HttpsRequestSetMaxRedirections(request, 0)`——HTTP 库不跟随任何重定向，把每一跳暴露给引擎。新平台若使用会自动跟随的 HTTP 库，**必须关掉它的自动重定向**，否则上述 Cookie 语义全部失效。

## 9.3 processResults：构建期的回压

结果在引擎线程落地（`processResults()`），但构建中的页面有特殊性：

- **构建进行中**（`m_buildPhase != BUILD_IDLE` 且非 post-swap 脚本运行）到达的图片/CSS 结果被**塞回队首**等待——半成品页不消耗挂载与重排；例外是 `BUILD_PRELOAD_CSS` 阶段的默认样式表（它正是状态机在等的东西）。
- **post-swap 脚本运行期间不算构建**：屏幕上的页面拥有缓存，CSS/图片必须继续落地，不能被一个可能跑很多拍的脚本堵住。
- HTML 结果落地 = 换页：`loadHtmlContent()` 清理构建资源、剥 `<script>`、启动 build 状态机（第 4 章），并先 `postUiEvent(EUET_URL)` 让 UI 刷新地址栏。
- 图片到达时若**两份文档都不存在**（竞态：页面刚被 STOP），结果带图重排队等待下次机会；既不重排也不挂载的位图必须显式 `surface_free`（所有权规则写在 `EWebResult` 的注释里）。

## 9.4 CookieJar：进程级、双线程、SameSite

`EWebCookieJar`（`src/EWebCookies.{h,cc}`）是**进程级单例**：一个进程里所有 ewebview 实例共享一个 Jar，两侧同时访问——下载线程存 `Set-Cookie`、引擎线程读写 `document.cookie`——所以全部入口走一把 `pthread_mutex`。单例经 `pthread_once` 惰性创建（构造器私有）：`pthread_mutex_init` 可能分配内核资源，不能发生在静态构造期。

**条目模型**（`EWebCookie`）：

| 字段 | 语义 |
| --- | --- |
| `domain` | 小写、**不带引导点**；是否匹配子域由 `hostOnly` 区分 |
| `hostOnly` | `Set-Cookie` 写了 `Domain=` → false（匹配子域）；没写 → true（只匹配该主机） |
| `path` | 恒以 `/` 开头 |
| `expiresMs` | 墙钟毫秒；**负数 = 会话 Cookie** |
| `sameSite` | `Lax`（默认，未声明即 Lax，与现代浏览器一致）/ `Strict` / `None` |
| `secure` / `httpOnly` | 仅 https 发送 / 对 DOM 不可见 |

**SameSite 判定**：`requestHeader(url, pageUrl, topLevel)` 三元组——`pageUrl` 是发起文档，`topLevel` 标记主文档导航本身。顶层导航允许 Lax Cookie 随行（用户点链接进来应带着登录态），子资源跨站时 Lax/Strict 都留下；`pageUrl` 为空（无发起者）按同源处理。

**站点近似的已知取舍**：`siteOf()` 用形状规则模拟 public suffix list——两字母 TLD 前是已知二级标签（`co/com/org/...`）就再吃一段（`www.shop.example.co.uk` → `example.co.uk`）。`github.io` 这类真正的 public suffix 识别不了，其子域之间会共享 Cookie。

**其他规则**：非 HTTP URL 用 scheme 当伪 host（`file`、`about`）+ 路径 `/`——所有本地文档共享一个 Cookie 桶，而不是按目录隔离；插入序即创建序，作为 RFC 6265 要求的路径等长 tie-break；**不落盘**，Jar 随进程死亡。

## 9.5 document.cookie 与 Web Storage

JS 两侧都汇到同一个 Jar / 同一对 blob：

- **`document.cookie`**：桥回调 `jsWebGetCookie/jsWebSetCookie` → `jar.jsGet/jsSet(jsDocumentUrl())`。`jsGet` 与 `requestHeader` 同一套匹配但**过滤 HttpOnly**（DOM 永远看不见）；`jsSet` 接受整串 Set-Cookie 语法（`Path/Domain/Max-Age/Expires/SameSite` 都生效）但**丢弃 HttpOnly**——只有服务器能设它。无 URL 的文档（`about:blank` 类）得到空的 Jar 视图。
- **`localStorage/sessionStorage`**：桥把全部条目序列化成**长度前缀的二进制安全 blob**，经 `storage_load/storage_save` 回调进出。blob 存在**引擎成员** `m_jsLocalStorage`/`m_jsSessionStorage` 上——这是故意的：VM 每次导航重建（第 6 章），而 storage 语义要求跨页面存活，所以数据必须比 VM 长寿。当前**不落盘**：`localStorage` 跨页不跨进程。

## 9.6 JS 的 HTTP：为什么 `http_request` 是 NULL

`js_web` 桥的 XHR/`fetch` 契约是**同步阻塞**（`http_request` 必须阻塞 VM 线程直到响应完成，见第 7 章）。ewebview 的引擎**故意不实现它**——`registerWebNatives()` 里该回调留 NULL：

> 本引擎的 HTTP 是异步任务队列（下载线程 → `m_resultQueue` → 引擎循环 `processResults()`），而 VM 就跑在引擎线程上。在 VM 里等一次下载，等于把**负责投递这个下载结果的循环**一起等死——死锁。

桥在钩子缺失时仍然安装 `XMLHttpRequest`、`fetch()`、`Response`、`Headers`，只是**每个请求都报告网络错误**（XHR `readyState 4`/`status 0`，`fetch` 得到 rejected Promise）而不是抛异常——这与浏览器对失败请求的行为一致，特性探测与错误处理路径都能正常走完。要支持真实 XHR，需要先把引擎的 HTTP 路径改造成可从引擎线程同步等待的形式（例如专用于 XHR 的独立 socket，不走任务队列）。

## 9.7 对嵌入者的可见性

嵌入者经监听器看到完整的加载活动（第 11 章）：`on_task_start/end/failed`（携带任务类型与 URL）、`on_tasks_end`（队列排空，可熄灭加载指示）、`on_url`（主文档落地）。xBrowser 用 `TASK_HTML` 结果做失败重试、用任务事件驱动进度指示，是这套信号的典型用法。
