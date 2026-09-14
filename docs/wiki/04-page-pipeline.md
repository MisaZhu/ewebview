# Chapter 4 · Page Load Pipeline (the build State Machine)

> Language: **English** | [中文](04-page-pipeline.zh.md)

Between `ewebview_load(url)` and a new page appearing on screen lie several stages: download, parse, style, script, layout. ewebview organizes them into a **non-blocking state machine**: driven by `m_buildPhase`, advanced one step per engine-loop round (`advanceBuildStep()`), and abortable by the UI at any time. This chapter breaks it down stage by stage.

## 4.1 State Overview

```c
enum BuildPhase {
    BUILD_IDLE = 0,        // no build: current page is live
    BUILD_PRELOAD_CSS,     // waiting for the UA default stylesheet (master.css) to be ready
    BUILD_CREATE_DOC,      // gumbo+litehtml parse HTML, build the tree
    BUILD_RUN_JS,          // run scripts (two modes, see 4.5)
    BUILD_RENDER_DOC,      // full style + layout
    BUILD_SWAP_DOC,        // new page replaces old page on screen
    BUILD_FAILED,          // failure wrap-up
};
```

Normal path:

```
engineNavigate() queues the HTML task
   → (HTML arrives) PRELOAD_CSS → CREATE_DOC → [RUN_JS] → RENDER_DOC → SWAP_DOC
   → [RUN_JS progressively runs remaining scripts] → IDLE
```

During construction the old page **always stays on screen**; `on_build_status(text, progress, overlay=true)` notifies the embedder to lay a progress overlay over it; only at the `BUILD_SWAP_DOC` instant do the old and new pages hand off.

## 4.2 Starting Point: engineNavigate()

`ECMD_NAVIGATE` / `ECMD_RELOAD` / script navigation all eventually enter `engineNavigate()` (engine thread):

1. `engineDropPendingWork()` — discard all queued/completed downloads (old-page resources are no longer needed);
2. `cleanupBuildResources()` — terminate the previous unfinished build (free the build document/container, reset the VM, clear canvases);
3. Zero the scroll, invalidate the frame cache;
4. **Rotate the litehtml context**: `m_browser_context` and `m_buildContext` alternate as a double buffer; the new page's master style set starts from scratch without disturbing the one the visible page is using;
5. Record the SameSite initiator (`setTaskPageUrl(m_currentHtmlUrl)`, see Ch. 9);
6. Queue the `EWEB_TASK_HTML` task.

Note that the **visible page `m_doc` is not released at this point** — it keeps showing on screen until it is replaced and lazily deleted at SWAP.

## 4.3 PRELOAD_CSS → CREATE_DOC

**BUILD_PRELOAD_CSS** (progress 15%): wait for the UA default stylesheet. The master.css specified by `ewebview_set_default_css()` loads before the first page; once the condition is met it advances to the next stage.

**BUILD_CREATE_DOC** (progress 45%):

- Create a new `EWebContainer` (binding the port and engine host), set the viewport size and the page base URL (the basis for resolving relative links);
- `setDeferImageLoad(true)`: during construction images only record URLs without issuing tasks (avoiding wasted downloads for a half-finished page);
- `litehtml::document::createFromString(html, container, ctx)` — **the longest single call in the whole flow** (gumbo parse + full DOM tree build). It runs on the engine thread so it cannot freeze the UI, but it must be **abortable**: hot callbacks like the container's `create_element`/`text_width` poll `buildAbortRequested()` (see 4.7) and short-circuit return once it is set, letting the parse unwind quickly;
- After parsing, `replayJsMutations()`: if this is a document.write-triggered reparse, replay the text/attr/title changed by the previous round's scripts onto the new tree (see Ch. 6);
- If there is `document.write` suspicion (`m_jsRunBeforePaint`) and there are scripts → `BUILD_RUN_JS` (run-scripts-first mode); otherwise go directly to `BUILD_RENDER_DOC`.

## 4.4 RENDER_DOC: Full Style + Layout

`BUILD_RENDER_DOC` (progress 80%) does two things:

1. **Full style computation**: the document is created in "fast mode" — `parse_styles` skips the entire box model, so dependencies like `100vh` and `attr(width)` collapse to 0. So before layout, fast mode is turned off and a full style traversal is forced. The traversal is **sharded**: `update_master_styles_step(deadline)` caps each shard at `kStyleBudgetIdleMs = 25ms`; between shards the engine returns to the main loop to drain commands / paint frames, and the abort flag can take effect mid-traversal.
2. **Layout**: `m_buildDoc->render(m_clientWidth)` computes all geometry. Then performance stats are printed (`text_width` calls/time/cache-hit rate, etc., visible under `EWEBVIEW_DEBUG`).

On completion it enters `BUILD_SWAP_DOC`.

## 4.5 The Two RUN_JS Modes

The timing of script execution is decided by whether the page might `document.write`:

**A. Run scripts first (pre-paint, `m_jsPostSwapRun == false`)**
Used only for pages that might write the document. It runs all inline scripts on the built DOM → `applyJsWriteBuffer()` splices the scripts' `document.write()` output **back into the HTML source** (inserted before `</body>`) → the state machine falls back to `BUILD_CREATE_DOC` to re-parse. Reparsing is capped at `kJsMaxReparse = 8`, and the script list is cleared on rebuild to prevent infinite loops.

**B. Run scripts progressively (post-swap)**
Ordinary pages **show on screen first, then run scripts**: after SWAP, each engine-loop round runs one script (`runNextPageScript()`); DOM modifications inside the script are painted out **mid-execution** via `jsMarkLayoutDirty() → jsProgressiveFlush()` — test-page results pop out one by one rather than a white screen waiting to the end. The progressive-relayout interval is adaptive: the previous flush cost ×3, clamped between `kJsFlushMinGapMs = 200` and `kJsFlushMaxGapMs = 1500`.

**External and dynamic scripts**: `<script src="...">` is no longer discarded — `extract_scripts()` leaves it an **empty-bodied ordered slot** (`m_jsScripts[i]` + `m_jsScriptSrcs[i]`, `m_jsScriptDone[i]=0`) and queues an `EWEB_TASK_SCRIPT` download in document order. When `runNextPageScript()` reaches a not-yet-ready slot it **returns "still running" but does not busy-wait**: it keeps `BUILD_RUN_JS` armed, the engine loop parks on the 4ms beat, until `processResults()` fills the slot (a failed download is also marked done + empty body, so a 404 does not stall ordered execution) and sets `m_deferBuildStep` to wake it. Scripts inserted dynamically while a script runs (`createElement('script'); s.src=...; appendChild` — this is how w3.org's members.js is added) go through `jsDynamicScriptInserted()`, which likewise appends an ordered slot and re-arms the run. Thus inline/external/dynamic scripts share the same strictly document-ordered execution flow.

At the convergence of both modes, `jsFireLoadEvents()` fires: first `DOMContentLoaded`, then document/window's `load`, then `<body onload>` — **only after the page scripts have run**, consistent with the order every page assumes.

## 4.6 SWAP_DOC: Old/New Page Handoff

`BUILD_SWAP_DOC` (progress 100%) is the pipeline's only "pointer move":

```c
m_doc       = m_buildDoc;        // the new document is promoted (same object, JS element handles stay valid)
m_container = m_buildContainer;
m_activeContext = m_buildTargetContext;
m_pendingDeleteDoc       = old_doc;        // old page deleted lazily next loop round
m_pendingDeleteContainer = old_container;  // (deleting now would re-enter a container still in use)
m_engineScrollX = m_engineScrollY = 0;     // new page returns to the top
m_cacheValid = false;                      // frame cache invalidated → repaint immediately
m_flushDeferredImages = true;              // images held back during construction now issue tasks
```

Then `postScrollClamp()` emits `EUET_SCROLL_CLAMP` so the UI syncs its scrollbar. If scripts remain unrun (progressive mode), the state machine stays in `BUILD_RUN_JS` to continue; otherwise it returns to `BUILD_IDLE` and the page enters its live phase.

`BUILD_FAILED`: entered when content is empty or parsing fails; the next round's `cleanupBuildResources()` wraps up.

## 4.7 Abort: Making a Half-Finished Page Die Fast

The user may click stop, go back, or type a new URL at any moment during download/parse/layout/script-run. The abort chain:

1. UI thread `ewebview_stop()` etc. → command enqueued; `requestBuildAbort()` raises `m_buildAbort` (volatile) and the **`m_buildAbortGen` generation counter**;
2. The litehtml currently parsing short-circuits out via `buildAbortRequested()` in the container's hot callbacks;
3. The mario VM currently running scripts is terminated by the step hook (`jsOnVmStep` compares the generation counter, see Ch. 6);
4. At the start of the next round's `advanceBuildStep()`, seeing `m_buildAbort` → `cleanupBuildResources()` discards the half-finished work (but **keeps** the just-queued pending navigation URL — that is the page the user actually wants).

**The generation counter is the key to precision**: it distinguishes "an abort that just arrived during this run" (unwind immediately, not counted as a script violation) from "a stale STOP consumed while idle" (which must not affect the current page script's watchdog count).

## 4.8 Layout Dirty Flags & Debouncing

Relayout during a page's live phase does not happen immediately; it goes through debounce-merging (`EWebInternal.h` constants):

| Constant | Value | Effect |
| --- | --- | --- |
| `kLayoutDebounceMs` | 30 ms | after marking dirty, wait at least this long, merging consecutive changes |
| `kLayoutMaxWaitMs` | 200 ms | the longest a dirty flag may hang; force relayout when reached |
| `kLayoutBehindStyleMs` | 500 ms | style backlog beyond this means it has fallen too far behind; take the compensation path |
| `kStyleMaxWaitMs` | 1500 ms | the cap for the master style traversal waiting on remaining `<link>` sheets (`m_pendingCss`); on timeout it starts anyway (a newly arrived sheet restarts the traversal from scratch) |
| `kStyleBudgetIdleMs` | 25 ms | the wall-clock budget of a single style shard |
| `kJsMaxReparse` | 8 | the document.write reparse cap |
| `kJsRunBudgetMs` | 10000 ms | the budget of a single VM run (Ch. 6) |

`applyPendingLayoutUpdates()` (step 4 of the engine loop) uses these watermarks to decide whether to actually run style/layout this round; only when the result is dirty does it set `m_contentDirty` to trigger a repaint. Rendering therefore **happens only when necessary**, and each render only paints without relayouting.

## 4.9 Progress & Status Reporting

| Stage | Text | progress | overlay |
| --- | --- | --- | --- |
| PRELOAD_CSS | "loading styles" | 15 | 1 |
| CREATE_DOC | "building document" | 45 | 1 |
| RUN_JS (run first) | "running scripts" | 60 | 1 |
| RENDER_DOC | "layout and first paint" | 80 | 1 |
| SWAP_DOC | "displaying page" | 100 | 1 |
| progressive scripts after SWAP | "running scripts" | 100 | **0** (page already on screen, only light the status bar) |
| complete/abort | "" | 0 | 0 |

The embedder draws the progress bar and overlay accordingly; only when `overlay` is true is the page masked (`on_build_status`).
