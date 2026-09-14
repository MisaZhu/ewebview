# Chapter 9 · Networking, Cookies & Storage

> Language: **English** | [中文](09-network-cookies.zh.md)

ewebview's networking subsystem solves three problems: how subresources come in **without blocking the engine thread**; how redirects and cookies stay safe when **cross-site**; and how JS's `document.cookie` and `localStorage` share state with the HTTP side. The core structure is only two queues, a download **worker pool**, and one process-level CookieJar.

## 9.1 The Subresource Task Queue & Download Worker Pool

```
Engine thread (producer)              Download worker pool (consumers, 0..8 on demand)
  addTask({url,type}) ──► m_taskQueue ──► getTaskLocked() takes one not-loading
        ▲                                    │ loadHtmlTask / loadCSSTask / loadImageTask / loadScriptTask
        │                                    │   └─ EWebContainer::loadURL() (may include redirects)
 m_resultQueue ◄── pushResult({url,ok,content,image})
        │
        ▼ engine-loop steps 2/3, processResults() (see Ch. 3)
   HTML→start the build state machine / CSS→loadCSSContent / image→mountDecodedImage / SCRIPT→fill the ordered script slot back
```

- **Task types** are the public constants `EWEB_TASK_HTML / EWEB_TASK_CSS / EWEB_TASK_IMAGE / EWEB_TASK_SCRIPT` (what the embedder sees via the `on_task_*` listeners; `EWEB_TASK_SCRIPT` is the download of an external `<script src>`, see Ch. 6).
- **`addTask` deduplicates by URL** (the same image appearing ten times on a page downloads once). The pool **starts empty** (`m_taskThreads == 0`); each `addTask` first **wakes a parked worker** (`pthread_cond_signal(m_taskCond)`), and only when every live worker is busy (`m_taskThreads - m_taskBusy <= 0`) does it **grow the pool** via `pthread_create` + `pthread_detach`, capped at `kTaskPoolMax = 8`. A worker with nothing to do parks on `m_taskCond` in bounded slices and **tears itself down once it has been idle for `kTaskIdleExitMs = 4000` ms**, so the pool shrinks back toward zero after a page's fetches drain; `EUET_TASKS_END` fires once when the queue empties and no worker is mid-fetch (`m_taskBusy == 0`).
- **The worker's discipline**: it touches only the mutex-protected `m_taskQueue`/`m_resultQueue` and the porting table's `net.*`/`image.decode` callbacks, **never the document or VM**; notifications to the UI (`EUET_TASK_START/END/FAILED`) all go through `postUiEvent`, delivered by the UI thread inside `ewebview_tick()` — the worker never calls embedder hooks directly. Because up to 8 workers run concurrently, everything they share (the queues, `m_taskPageUrl`, the CookieJar, and the port's `net.request`/`image.decode`) is either mutex-guarded or per-call reentrant.
- **topLevel semantics**: `loadHtmlTask` passes `topLevel=true` (a top-level navigation), CSS/images pass `false` (subresources). This boolean, together with `pageUrl` (the URL of the initiating document), drives the SameSite decision (see 9.4).
- **Images decode on the worker thread**: `loadImageTask` runs `decodeImageData()` in place after downloading (pure heap operations); by the time the engine thread receives the result the bitmap is ready, and it only does the O(1) `mountImage` mount — tens of milliseconds of decoding for a single image will not stall layout.

## 9.2 loadURL: Redirect Following & Per-Hop Cookie Scoping

All entry points (HTML/CSS/images, and `file://`) funnel into `EWebContainer::loadURL()`. `file://` goes through `net.read_file`; `http(s)://` goes through `net.request`/`net.free_response`, and **redirects are followed on the engine side** (capped at `kMaxRedirects = 5` hops) rather than letting the HTTP library follow them internally. Three reasons:

1. **Cross-site cookie leakage**: internal following would carry the first hop's `Cookie:` header verbatim to every host after the redirect. Engine-side following instead **recomputes** the Cookie header per hop — `EWebCookieJar::requestHeader(cur_url, pageUrl, topLevel)` admits only the entries allowed by this hop's domain/path/Secure/SameSite rules, so when jumping to an external site the old site's cookies naturally stay home.
2. **Lost Set-Cookie**: internal following keeps only the last hop's response headers. The engine side stores this hop's entire `Set-Cookie` into the Jar **before any processing** — a login flow sets its session cookie precisely on a 302, and an error-status response may carry one too.
3. **Relative Location**: `Location: /login` needs to be resolved against "the URL that initiated this hop"; internal following gets the final URL but loses the intermediate context.

Landing rules: a failed request, `resp.error`, a non-200 status, or an empty body all return NULL (a failed result also enters `m_resultQueue`, and the engine degrades by type — a CSS failure calls `forgetCSS`, an image failure keeps the 0×0 layout).

**Porting-layer cooperation**: both reference ports (`port_ewokos.c` and `port_sdl2.c`) explicitly `HttpsRequestSetMaxRedirections(request, 0)` — the HTTP library follows no redirect, exposing every hop to the engine. If a new platform uses an HTTP library that follows automatically, it **must turn off its auto-redirect**, otherwise all the cookie semantics above fail.

## 9.3 processResults: Backpressure During Construction

Results land on the engine thread (`processResults()`), but a page under construction has special properties:

- Image/CSS results arriving **while a build is in progress** (`m_buildPhase != BUILD_IDLE` and not a post-swap script run) are **pushed back to the queue front** to wait — a half-finished page does not consume mounts and relayouts; the exception is the default stylesheet during `BUILD_PRELOAD_CSS` (it is exactly what the state machine is waiting for).
- **A post-swap script run does not count as construction**: the on-screen page owns the cache, and CSS/images must keep landing, not be blocked by a script that may run for many beats.
- An HTML result landing = a page change: `loadHtmlContent()` cleans up build resources, strips `<script>`, starts the build state machine (Ch. 4), and first `postUiEvent(EUET_URL)` so the UI refreshes its address bar.
- If **neither document exists** when an image arrives (a race: the page was just STOPped), the result is re-queued with its image to wait for the next chance; a bitmap that is neither relayouted nor mounted must be explicitly `surface_free`d (the ownership rule is written in `EWebResult`'s comments).

## 9.4 CookieJar: Process-Level, Two-Threaded, SameSite

`EWebCookieJar` (`src/EWebCookies.{h,cc}`) is a **process-level singleton**: all ewebview instances in a process share one Jar, accessed from both sides at once — the download thread stores `Set-Cookie`, the engine thread reads/writes `document.cookie` — so all entry points go through one `pthread_mutex`. The singleton is lazily created via `pthread_once` (private constructor): `pthread_mutex_init` may allocate kernel resources and must not happen during static construction.

**Entry model** (`EWebCookie`):

| Field | Semantics |
| --- | --- |
| `domain` | lowercase, **without a leading dot**; whether subdomains match is distinguished by `hostOnly` |
| `hostOnly` | `Set-Cookie` wrote `Domain=` → false (matches subdomains); didn't → true (matches only that host) |
| `path` | always starts with `/` |
| `expiresMs` | wall-clock milliseconds; **negative = session cookie** |
| `sameSite` | `Lax` (default; undeclared means Lax, consistent with modern browsers) / `Strict` / `None` |
| `secure` / `httpOnly` | sent only over https / invisible to the DOM |

**SameSite decision**: the `requestHeader(url, pageUrl, topLevel)` triple — `pageUrl` is the initiating document, `topLevel` marks the main-document navigation itself. A top-level navigation allows Lax cookies to travel along (a user clicking a link in should carry their login state); a cross-site subresource leaves both Lax/Strict behind; an empty `pageUrl` (no initiator) is treated as same-origin.

**Known trade-off in the site approximation**: `siteOf()` simulates the public suffix list with shape rules — if a known second-level label (`co/com/org/...`) precedes a two-letter TLD it eats one more segment (`www.shop.example.co.uk` → `example.co.uk`). Genuine public suffixes like `github.io` are not recognized, so their subdomains share cookies.

**Other rules**: non-HTTP URLs use the scheme as a pseudo-host (`file`, `about`) + path `/` — all local documents share one cookie bucket rather than being isolated per directory; insertion order is creation order, serving as the equal-path-length tie-break required by RFC 6265; **not persisted to disk**, the Jar dies with the process.

## 9.5 document.cookie & Web Storage

Both JS sides funnel into the same Jar / the same pair of blobs:

- **`document.cookie`**: the bridge callbacks `jsWebGetCookie/jsWebSetCookie` → `jar.jsGet/jsSet(jsDocumentUrl())`. `jsGet` uses the same matching as `requestHeader` but **filters HttpOnly** (the DOM never sees them); `jsSet` accepts the whole Set-Cookie syntax (`Path/Domain/Max-Age/Expires/SameSite` all take effect) but **discards HttpOnly** — only the server can set it. A document with no URL (an `about:blank` kind) gets an empty Jar view.
- **`localStorage/sessionStorage`**: the bridge serializes all entries into a **length-prefixed binary-safe blob**, entering and leaving via the `storage_load/storage_save` callbacks. The blob lives on **engine members** `m_jsLocalStorage`/`m_jsSessionStorage` — this is deliberate: the VM is rebuilt every navigation (Ch. 6), while storage semantics require surviving across pages, so the data must outlive the VM. Currently **not persisted to disk**: `localStorage` survives across pages but not across processes.

## 9.6 JS's HTTP: Why `http_request` Is NULL

The `js_web` bridge's XHR/`fetch` contract is **synchronous blocking** (`http_request` must block the VM thread until the response completes, see Ch. 7). ewebview's engine **deliberately does not implement it** — the callback is left NULL in `registerWebNatives()`:

> This engine's HTTP is an async task queue (download thread → `m_resultQueue` → engine loop `processResults()`), and the VM runs on the engine thread. Waiting for a download inside the VM means waiting to death on **the very loop responsible for delivering that download's result** — a deadlock.

When the hook is missing the bridge still installs `XMLHttpRequest`, `fetch()`, `Response`, `Headers`, except **every request reports a network error** (XHR `readyState 4`/`status 0`, `fetch` gets a rejected Promise) rather than throwing — consistent with a browser's behavior for a failed request, so feature detection and error-handling paths run to completion normally. To support real XHR, the engine's HTTP path would first need to be reworked into a form synchronously waitable from the engine thread (e.g. a dedicated socket for XHR that does not go through the task queue).

## 9.7 Visibility to the Embedder

The embedder sees the full loading activity via listeners (Ch. 11): `on_task_start/end/failed` (carrying the task type and URL), `on_tasks_end` (queue drained, the loading indicator can be turned off), `on_url` (main document landed). xBrowser uses the `TASK_HTML` result for failure retry and the task events to drive its progress indicator — a typical use of these signals.
