# Chapter 3 · Threading Model & Frame Delivery

> Language: **English** | [中文](03-threading.zh.md)

ewebview's concurrency design answers three questions: how the UI never blocks, how the two background threads divide labor, and how render results cross threads onto the screen with zero copy. All the rules are written into the comment contracts of `ewebview.h` and `ewebview_port.h`; this chapter expands the implementation side.

## 3.1 The Three Thread Classes

```
┌──────────────────────┐
│ UI thread (embedder-  │  ewebview_load/stop/scroll/post_event  → post commands
│  owned)               │  ewebview_tick()                         → collect events, fire listener callbacks
│                      │  ewebview_release_frame()                → return frames
└────────┬─────────────┘
         │ m_cmdQueue (pthread_mutex+cond)
         ▼
┌──────────────────────┐
│ Engine thread (1 per  │  Owns exclusively: 2 litehtml documents, 2 containers,
│  instance)            │  the mario VM, the surface being painted, the canvas registry
│  engineLoop()        │
└────────┬─────────────┘
         │ m_taskQueue / m_resultQueue (one mutex each)
         ▼
┌──────────────────────┐
│ Download thread       │  net.request / net.read_file / image.decode
│  (1 on demand)        │  touches only the task and result queues, exits when done
│  taskLoop()          │
└──────────────────────┘
```

- The **engine thread** is created at `ewebview_create()` time (`engineStart()`); after creation it waits on `m_cmdCond` until the first command arrives. It **owns exclusively** all non-thread-safe objects: the two litehtml documents (visible page + page under construction), the two litehtml contexts, the mario VM, and the surface it is currently painting. **It never touches the embedder's window.**
- The **download thread** is spun up on demand (`addTask()` calls `pthread_create` for a `taskLoop()` when it finds no live worker); it exits on its own once the queue drains, and a new one starts the next time there is a task.
- The **UI thread** is the embedder's own main loop. Between it and the engine there are only two channels: the command queue (UI→engine) and the event queue (engine→UI), plus the ownership handoff of frame-pool pointers.

## 3.2 UI → Engine: The Command Queue

Every action of the public API merely **queues a command and returns immediately** (`ewebview.h` comment: the window never blocks on download/parse):

| Public API | Command | Payload |
| --- | --- | --- |
| `ewebview_load(url)` | `ECMD_NAVIGATE` | url |
| `ewebview_stop()` | `ECMD_STOP` | — |
| `ewebview_reload()` | `ECMD_RELOAD` | — |
| `ewebview_set_viewport(w,h)` | `ECMD_RESIZE` | w,h |
| `ewebview_scroll(x,y)` | `ECMD_SCROLL` | x,y |
| `ewebview_post_event(&ev)` | `ECMD_INPUT` | a copy of `eweb_event_t` |
| `ewebview_set_js_enabled(b)` | `ECMD_SET_JS` | b |
| (internal, on destruction) | `ECMD_SHUTDOWN` | terminates the engine loop |

The four **termination triggers** (STOP / NAVIGATE / RELOAD / SHUTDOWN) all perform cleanup on the engine thread (`engineHandleCommand`) — documents, the VM, and surfaces are destroyed by the very thread that owns them; there is no cross-thread delete.

`postCommand()` may be called by any thread: it takes `m_cmdMutex` to enqueue and `pthread_cond_signal(&m_cmdCond)` to wake the engine.

## 3.3 Engine → UI: The Event Queue and tick()

The engine produces `EWebUiEvent` (`EWebInternal.h`):

| Event | Corresponding listener hook | Meaning |
| --- | --- | --- |
| `EUET_FRAME` | `on_frame(frame, sx, sy, doc_w, doc_h)` | A frame finished rendering, **ownership transferred** |
| `EUET_SCROLL_CLAMP` | `on_scroll(x, y, doc_w, doc_h)` | The engine's authoritative scroll value (zeroed on page change, scrollTo, etc.) |
| `EUET_URL` | `on_url(url)` | The visible page URL changed (including in-page navigation) |
| `EUET_BUILD_STATUS` | `on_status` / `on_build_status(text, progress, overlay)` | Build progress and overlay |
| `EUET_TITLE` | `on_title(title)` | `<title>` or `document.title` |
| `EUET_DIALOG` | `on_dialog(text)` | alert/confirm/prompt text (non-blocking) |
| `EUET_TASK_START/END/FAILED/TASKS_END` | `on_task_*` | Subresource task lifecycle (progress bar/spinner) |

`ewebview_tick()` is the **only** place that calls back into the embedder, guaranteeing all listeners run on the UI thread. Its implementation has two key details (`ewebview.cc` `EWebEngine::tick()`):

1. **Move everything out first, then call back**: under `m_uiMutex` it copies the whole queue out and snapshots the listener table, then fires callbacks one by one **outside the lock** — the embedder's callback is free to call engine APIs again (e.g. `load()` inside `on_url`) without deadlock.
2. **Switching listeners mid-flight is safe**: `ewebview_set_listener()` likewise copies by value under `m_uiMutex`; the snapshot already taken this tick keeps using the old table, and the new one takes effect next tick.

The embedder's obligation: **call `ewebview_tick()` once per UI beat** (WidgetWebview calls it in `onTimer`).

## 3.4 Frame Pool: Ownership Transfer + Backpressure

Frame delivery is the most delicate part of this design, aiming for **zero pixel copy across threads**:

```
Engine                        UI
 │  engineEnsureFramePool()    │
 │  take a surface from         │
 │    m_freeFrames              │
 │  rasterize the whole viewport│
 │  m_pendingFrame = buf        │
 │  emit EUET_FRAME ──────────► │ tick(): frame = m_pendingFrame
 │                             │         m_pendingFrame = nullptr
 │                             │ on_frame(frame) → embedder 【claims】
 │                             │   (kept as a display cache, blitted on repaint)
 │                             │ after use, ewebview_release_frame()
 │  ◄──────────── returns to m_freeFrames
```

Rules:

- **Ownership transfers with `on_frame`**. The embedder usually stores it as a display cache and returns the previous frame to the pool via `ewebview_release_frame()` **before claiming the next one**.
- **The engine does not touch the surface until it is returned** — natural backpressure: if the UI has not yet claimed the previous frame (`m_pendingFrame != nullptr`), `engineRenderFrame()` simply skips this render; the frame pool never grows, and there is no per-frame allocation.
- The frame event carries the **scroll offset at render time** `(frame_scroll_x, frame_scroll_y)` and the full document size `(doc_w, doc_h)`. During fast scrolling the UI's live scroll value may lead the rendered value; the embedder just blits the frame translated by `frameScroll - liveScroll` to avoid a tearing feel (see Ch. 11).
- Returning a frame does not check size: per contract the `gfx` table is engine-thread-only, and size validation happens on the engine thread inside `engineEnsureFramePool()` (after a resize, old-sized surfaces are discarded and rebuilt).
- Both `tick()` and `releaseFrame()` `pthread_cond_signal(&m_cmdCond)` after handling a frame, immediately waking the engine from its "waiting to be claimed" parked state to paint the next frame.

## 3.5 Engine Main Loop (engineLoop)

`EWebEngine::engineLoop()` in `ewebview.cc` does ten things per round, the order being the priority:

```
1. Drain the command queue (exit the loop on SHUTDOWN)
2. Land the deferred deletions left over from the last swap (old document/old container)
3. Drain the download result queue, processResults()
4. Handle style/layout dirty flags of the visible and build pages, applyPendingLayoutUpdates()
5. Advance the build state machine one step, advanceBuildStep()
6. Execute script-requested navigation/scrolling, jsRunPendingNavigation()
7. Poll JS timers, jsPollTimers() (mark dirty if any callback fired)
8. Mount images deferred during construction, flushPendingImages()
9. Page changed or scroll offset changed → engineRenderFrame()
10. If nothing to do, park with a time limit (see below)
```

**Parking strategy** (zero CPU when idle):

| State | Timeout |
| --- | --- |
| A frame awaiting UI claim | 16 ms |
| Building / dirty flags / pending work | 4 ms (yields but nearly spins) |
| Page idle but VM alive | 16 ms (timer granularity) |
| Fully idle | 200 ms |

Waits are always **bounded** `pthread_cond_timedwait` — a lost signal is at most one timeout late, never sleeping forever; and `postCommand()` / `pushResult()` / `releaseFrame()` all signal, so real events wake it immediately. Lock-order convention: `m_resultMutex` and `m_cmdMutex` are never nested, so there is no deadlock with the worker/UI.

After the loop exits (SHUTDOWN received), `engineTeardown()` releases the documents, VM, canvases, and frame pool **in the engine thread's own context**, then the thread returns — the destructor then `pthread_join`s and destroys the mutexes. This is the entire basis for "`ewebview_destroy()` is safe".

## 3.6 The Porting Layer's Threading Contract

`ewebview_port.h` annotates which thread calls each table; a port must obey:

| Table | Calling thread | Requirement |
| --- | --- | --- |
| `gfx.*` | engine thread only | no internal lock needed |
| `font.*` | engine thread only | no internal lock needed |
| `image.decode` | download thread only | pure heap operations, must not touch engine state |
| `net.*` | download thread only | each request independent, reentrant |
| `clock.tic_ms` | engine + download threads | reentrant |
| `sys.log` | engine thread | — |
| `sys.ptr_sane` | engine thread | cheap, no dereference |

The primitives used by the EwokOS reference port (graph/font/tinyhttpsc/kernel_tic) and by the SDL2 reference port (SDL2/SDL2_ttf/SDL2_image/SDL2_gfx + the same tinyhttpsc) keep no cross-call state; SDL surfaces/renderers are themselves not thread-safe, but each surface is touched only by the thread that created it, naturally satisfying the constraints above.

## 3.7 Why No More Locks Are Needed

- litehtml documents, the mario VM, canvases: single-thread (engine) ownership, zero locks;
- CookieJar: shared by the download thread (storing Set-Cookie) and the engine thread (document.cookie), **carries its own mutex** (`EWebCookies.h`);
- Task/result queues: one mutex each, worker and engine take locks without nesting;
- Frame pool: only pointers are handed off, one narrow `m_uiMutex`;
- Listener table: read/written by the UI thread + `m_uiMutex` protecting the engine side's snapshot when posting events.

The overall lock count is minimal, all one-directional with no lock-order cycles — a direct benefit of the "engine-exclusive + queue-communication" model.
