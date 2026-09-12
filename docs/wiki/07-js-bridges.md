# 第 7 章 · JS 桥接层：DOM / Event / Web

`jsnative/natives/` 下的四个文件是 mario VM 与引擎之间的**桥**（bridge）。它们纯 C、平台无关、不认识 litehtml——每个桥定义一张回调表（`js_*_callbacks_t`），由引擎（`EWebJs.cc` / `EWebCanvasGlue.cc` 里的静态成员）实现。JS 调 DOM 方法 → 桥翻译成回调 → 引擎操作 litehtml 文档，这条路是 HTML/CSS/JS 协同的主干道。

```
JS 脚本: document.getElementById("x").textContent = "hi"
   │
   ▼ js_dom.c native（纯 C，mario 对象操作）
   js_dom_callbacks_t.get_element_by_id / el_set_text   ← 回调表
   │
   ▼ EWebJs.cc: jsGetElementById / jsElSetText（引擎静态成员）
   litehtml::document / element（引擎线程，同一棵树）
   │
   ▼ jsMarkLayoutDirty() → 标脏 → 引擎重排 → 重绘新帧
```

## 7.1 注册顺序与共享上下文

`initJsVm()` 中的注册顺序是**强制**的（后面的桥要查前面的桥注册的类与全局对象）：

```
vm_init(vm, reg_all_natives)      // mario 内建类（Object/Array/Console/...）
js_register_dom_natives(vm, ctx, &dom_cb)   // Document/Element + window/document 全局
registerCanvasNatives(vm)                   // 挂 getContext 到 Element 类
js_register_event_natives(vm)               // Event/EventTarget + on* 属性
js_register_web_natives(vm, &web_cb)        // location/navigator/storage/XHR...
```

四个桥共享**同一个嵌入者上下文** `ctx`（即 `EWebEngine*`）：DOM 桥最先注册并保存它，其余桥通过 `js_dom_ctx(vm)` 取回，避免重复穿线。字符串返回值一律 `mario_malloc()` 分配、桥用完 `mario_free()`——契约写在每个头的注释里。

## 7.2 js_dom：Document / Element 桥

`js_dom.h` 的 `js_dom_callbacks_t` 是 DOM 能力的全集（节选）：

| 回调 | 服务的 JS API |
| --- | --- |
| `alert` / `document_write` | `alert()`、`document.write/writeln` |
| `get_title`/`set_title`/`get_url` | `document.title`、`window.location.href` 读 |
| `get_element_by_id` | `document.getElementById` |
| `el_get/set_text`、`el_get/set_html`、`el_get/set_attr`、`el_get_tag`、`el_remove_attr` | `textContent/innerText/innerHTML`、属性族、`tagName` |
| `el_is_live` | 句柄活性门（见 6.6） |
| `get_root`/`get_body`/`get_head` | `documentElement/body/head` |
| `query_all` | `querySelector(All)`、`getElementsBy*`、`matches/closest`（skip/max 分页协议，一次最多 `JS_DOM_QUERY_CHUNK=32`） |
| `create_element`/`create_text_node` | `document.createElement/createTextNode` |
| `el_parent/child_count/child/is_tag` | 树遍历（`children` vs `childNodes` 靠 is_tag 区分） |
| `el_append_child/insert_before/remove_child` | 树的增删（布局失效由引擎负责） |
| `el_get_rect`/`el_get_style`/`el_focus`/`el_scroll_into_view` | 几何、计算样式、焦点 |

**元素包装缓存**：桥为每个元素句柄缓存一个 JS 包装对象（同一节点多次查到同一对象）；文档重建时必须 `js_dom_reset_element_cache()`，否则包装里是指向已释放内存的句柄。

**定时器**：`setTimeout/setInterval/requestAnimationFrame` 由桥内的定时器表实现，桥本身不带时钟——引擎每拍把 `tic_ms()` 传进 `js_dom_poll_timers(vm, now_ms)`，桥按增量触发到期回调并返回触发个数（>0 引擎就标脏重绘）。

**document.write**：write 内容先累积在桥内缓冲，引擎在脚本退出后用 `js_dom_take_write_buffer()` 取走并重解析（见 6.5）。

## 7.3 js_event：Event / EventTarget 桥

`js_event.h` 提供完整的事件模型：`Event/CustomEvent/MouseEvent/KeyboardEvent` 构造器、全字段（`clientX/pageX/screenX/offsetX/button/buttons/keyCode/mods...`）、`preventDefault/stop(Immediate)Propagation`、以及 **捕获→目标→冒泡**的完整传播。`addEventListener` 支持 capture 与 once；on\* 属性（`onclick` 等）在 Element/Document/window 上都可赋值；HTML 内联属性（`<div onclick="...">`）按需编译并以 `this`=元素、`event` 绑定执行。

**输入事件的完整路径**（引擎侧 `jsDispatchMouseEvent()`）：

```
UI 线程: ewebview_post_event(EWEB_MOUSE_DOWN, LEFT, cx, cy)
   │  ECMD_INPUT 入队
   ▼ 引擎线程
EWEB_MOUSE_* → "mousedown/mouseup/click/dblclick/mousemove"（wheel 无 DOM 事件）
EWEB_BUTTON_* → DOM button（0 左 / 1 中 / 2 右）
   │
   ▼ 命中测试（文档坐标 = client + 引擎滚动偏移；fixed 元素用 client 对）
root->get_element_by_point(cx+scrollX, cy+scrollY, cx, cy)
   │
   ▼ hover 跟踪：目标变了先 mouseout 旧元素再 mouseover 新元素
js_event_dispatch_mouse(vm, target, type, ...)  ← 全传播在桥内完成
   │
   ▼ 返回 false（某监听器 preventDefault）→ ECMD_INPUT 处理方跳过默认动作
```

**链接点击**（`handleAnchorClick`，引擎线程）：

1. **8px 抖动门限**：按下与抬起位移超过 8px 视为拖滚而不是点击，不导航；
2. 命中测试用**可见页** `m_doc`（用户点的是屏幕上的东西，不是构建中的页）；
3. 从命中元素**向上走到最近的 `<a href>`**（命中的通常是锚里的文本节点）；
4. `container->on_anchor_click(href)` → 引擎记 `m_jsPendingNav` 并置构建中止 → 引擎循环第 6 步 `jsRunPendingNavigation()` 真正跳转。脚本里 `location.href = ...` 也汇到同一个待导航槽。

事件回调跑在引擎线程的 VM 里，因此**不能就地拆页**——导航、滚动都记为 pending，由主循环在脚本退绕后执行。

## 7.4 js_web：BOM（window 上的其他一切）

`js_web.h` 覆盖页面对 `window` 的全部日常期待，回调表对应关系：

| 回调 | JS 表面 |
| --- | --- |
| `confirm`/`prompt` | `confirm()`（非阻塞，默认 cancel）/`prompt()`（默认 null） |
| `get_viewport`/`get_screen` | `innerWidth/outerWidth`、`screen.width/colorDepth/orientation` |
| `get_scroll`/`scroll_to` | `scrollX/pageXOffset`、`scrollTo/scrollBy` |
| `navigate`/`reload`/`history_*` | `location.assign/href=`、`history.back/go/length` |
| `get_user_agent`/`get_language`/`get_platform` | `navigator.*`（NULL 时用内建默认，特性探测脚本不会抛） |
| `get_cookie`/`set_cookie` | `document.cookie`（接 CookieJar，第 9 章） |
| `storage_load`/`storage_save` | `localStorage/sessionStorage` 持久化（长度前缀二进制安全 blob） |
| `http_request` | **同步** XHR 与 `fetch()`（Promise 包在同一回调上）；**ewebview 引擎把它留 NULL**——同步阻塞会与异步任务队列死锁，所有请求报告网络错误（见 9.6） |

引擎侧补充的状态：storage 的两个 blob（`m_jsLocalStorage` / `m_jsSessionStorage`）**故意比 VM 活得久**（VM 每次导航重建，storage 不丢）；`scrollTo` 记 `m_jsScrollPending` 由主循环执行；`alert()` 文本经 `EUET_DIALOG` 送到 UI 的状态栏，**永不阻塞**等模态。

## 7.5 js_canvas：Canvas 2D 桥

`js_canvas.h` 的 `js_canvas_callbacks_t` 把 `CanvasRenderingContext2D` 的**全部状态与几何**（CTM、路径、弧线/贝塞尔细分、扫描线填充、虚线、渐变、阴影）留在桥内，只把最终图元以设备坐标发给引擎：`fill_rect`、`draw_line`、`fill_circle`、`arc`、`round`、`stroke_quadratic/bezier`、`set/get_pixel`、`blit`、`draw_text`、`set_clip`。详细见第 8 章。

## 7.6 桥的渐进采用与降级

每张表的**每个回调都是 OPTIONAL**：NULL 回调让对应 API 按"缺失特性"的方式退化（返回 null/""/0/false）而不是崩溃。`js_web` 甚至允许整表为 NULL——API 照样安装、全部报默认值，所以探测型脚本（`if (window.X) ...`）不会抛异常。这使得新平台的桥也可以增量点亮（第 10 章的移植顺序同理）。

## 7.7 已知留白

- `window.postMessage()` 接受调用但无操作（无 iframe/worker/popup）；
- `globalThis` 别名 `window`，与 VM 真正的全局（`vm->root`）不是同一对象——读全局可以，经它新建全局对裸标识符不可见；
- `performance.mark()/measure()` 不保留时间线；
- `crypto.getRandomValues()` 用单调时钟播种，只能做 id，不能做密钥；
- 桥的 XHR 契约恒同步（`open()` 的 async 参数被忽略），`fetch` 的 Promise 在其上封装；而 ewebview 引擎未实现 `http_request` 回调，XHR/`fetch` 当前**总是以网络错误告终**（XHR `status 0` / fetch rejected），不抛异常（见 9.6）。
