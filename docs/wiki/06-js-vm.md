# Chapter 6 · JavaScript: mario VM Integration & Script Scheduling

> Language: **English** | [中文](06-js-vm.zh.md)

ewebview's JS is executed by the **mario VM** (the `mario_js/` submodule, a pure-C bytecode virtual machine + JS frontend). litehtml itself has no scripting capability, so the engine strips `<script>` out of the HTML before it enters the parser, then feeds it to the VM in stages across the page lifecycle. This chapter covers the VM lifecycle, script-scheduling timing, the watchdog, and document.write reparsing.

The mario VM's own internal design (bytecode, compiler, GC) has separate documentation: [mario_js/docs/wiki/](../../mario_js/docs/wiki/README.md); this chapter only covers "how the engine uses it".

## 6.1 Script Extraction: Stripping `<script>` Before Parsing

`extract_scripts(html, &scripts, &script_srcs, &has_inline_handlers)` in `ewebview.cc`:

- Scans the whole text and **removes** each `<script>...</script>` block from the HTML (litehtml does not know JS; `el_script` would leave a piece of text that renders as nothing);
- **Inline scripts**: a non-empty script body is appended to `scripts` in document order, with the corresponding `script_srcs` position left as an empty string;
- **External scripts** `<script src="...">`: **now supported** — an empty-body slot is appended to `scripts` and its `src` to `script_srcs` (the two arrays are equal length); the caller resolves it to an absolute URL then queues an `EWEB_TASK_SCRIPT` download, filling the slot back once it arrives (see 6.3);
- Blocks whose `type` exists and does not contain `javascript` (e.g. `application/json`, `module`) are skipped, not executed;
- Attribute names are case-insensitive, values case-sensitive (URLs are case-sensitive); `src=` only matches the full attribute name, so `data-src=`/`srcset=` are not mis-matched;
- It also reports whether the page carries **inline event attributes** (`onclick="..."`, etc.) — even without a `<script>` block, a page with inline events still needs the VM.

Why strip before parsing: the script execution timing (after the document is built) must be separated from the parse timing.

## 6.2 VM Lifecycle

**Creation (`initJsVm()`, lazy)** — the VM is built only when the page satisfies both "JS enabled + has scripts or has inline events". A purely markup page never pays the VM's memory cost:

```
vm_new(js_compile, ...)          // hook the compiler function pointer into the VM
vm_init(vm, reg_all_natives, ..) // install builtin classes: Object/Array/String/Console/...
vm->step_interval = 32768        // call the step hook once every 32768 instructions
vm->on_step       = jsVmStepHook // watchdog + abort check (see 6.4)
js_register_dom_natives(vm, this, &cb)   // the four bridges register in order (Ch. 7)
registerCanvasNatives(vm)
registerEventNatives(vm)
registerWebNatives(vm)
```

**mario's platform hooks** are three process-level function pointers: `_platform_malloc` / `_platform_free` / `_platform_out`. The engine wires them to libc malloc/free and the port's `sys.log` (JS `console.log` thereby outputs with a `[js]` prefix). Because `_platform_out` has no ud parameter, the engine uses a file-level static pointer to the log hook of "the engine that last ran the VM" — the correct scope for a single-browser scenario.

**Destruction**: `vm_close()` frees the VM and all its objects. Every navigation (page change) rebuilds the VM — JS global state does **not** persist between pages (what persists is cookies and localStorage, see Ch. 9).

## 6.3 Script-Scheduling Timing & Order

```
CREATE_DOC ──► (might document.write?) ──yes──► RUN_JS (run first, see Ch. 4, 4.5A)
                    │no                              │ document.write output spliced back into HTML
                    ▼                                ▼ back to CREATE_DOC (capped at 8)
              RENDER_DOC → SWAP_DOC (on screen first)
                    │
                    ▼ RUN_JS (progressive, 4.5B): one script per engine-loop round
              all done → jsFireLoadEvents() → IDLE
```

- **Multiple scripts share the global scope**: `vm_load_run()` executes the current script's bytecode appended after the previous one, with globals stored in `vm->root` — consistent with multiple `<script>` blocks in a browser sharing one global scope;
- **External/dynamic scripts run in the same order**: the three equal-length arrays `m_jsScripts` / `m_jsScriptSrcs` / `m_jsScriptDone` describe the ordered script slots. An external slot has `done=0` until its `EWEB_TASK_SCRIPT` result arrives; when `runNextPageScript()` meets it, it returns "still running" without busy-waiting (the engine parks on the 4ms beat and wakes immediately when `pushResult` arrives), thus executing strictly in document order; scripts dynamically inserted while a script runs via `createElement('script')+appendChild` go through `jsDynamicScriptInserted()` to append a new slot (external ones also use `EWEB_TASK_SCRIPT`) and re-arm the run;
- **Progressive visibility**: in post-swap mode, a script's DOM modifications are relayouted and repainted mid-run via `jsProgressiveFlush()` (adaptive interval: previous flush cost ×3, clamped to 200~1500ms);
- **load events fire last**: `DOMContentLoaded` → document/window `load` → `<body onload>`, ensuring listeners see a laid-out, on-screen document;
- **Live phase**: thereafter a script's entry points are only two kinds — timers (step 7 of the engine loop, `jsPollTimers()` → `js_dom_poll_timers(vm, ticMs())`) and input events (`jsDispatchMouseEvent()`, Ch. 7).

**Which document** a script operates on is decided by `jsActiveDoc()`: `m_buildDoc` during construction, `m_doc` after it is on screen. `BUILD_SWAP_DOC` hands off the same document object, so element handles held by scripts stay valid across the swap.

## 6.4 The Run-Budget Watchdog

A script infinite loop must not lock up the browser. Three levels of protection:

1. **step hook**: the VM calls `jsOnVmStep()` every 32768 instructions;
2. **run budget**: a single VM run (a page script, a timer callback, an event handler) exceeding `kJsRunBudgetMs = 10000ms` → sets `vm->terminated = true`, and the VM unwinds throughout;
3. **counting elimination**: `jsVmExit()` finds this run was terminated by the watchdog, `m_jsAbortCount++`; after `kJsRunAbortMax = 3` accumulated → `m_jsPageDisabled = true`, **this page's JS is shut off entirely** until the next navigation.

**Generation counting prevents false positives**: `jsVmEnter()` snapshots `m_buildAbortGen`. If a run's termination is because the user clicked stop/back (the generation counter changed during the run), that termination is **not** counted as a violation — only a genuine timeout counts. `jsVmExit()` also does `vm_terminate()` to clean up the operand/scope residual frames left by the unwind, so the VM can serve the next callback.

## 6.5 document.write: Reparsing & State Survival

`document.write()` in a modern engine is parser-insertion-point semantics; ewebview approximates it with **whole-page reparsing**:

1. the js_dom bridge accumulates the script's write content into a buffer, drained by `js_dom_take_write_buffer()`;
2. `applyJsWriteBuffer()` **inserts the content before `</body>`** (appending to the end if there is none);
3. reparse cap `kJsMaxReparse = 8`; on rebuild the **script list is cleared** to prevent infinite loops;
4. before rebuilding the document, `jsInvalidateHandles()`: `js_event_clear_listeners` + `js_dom_reset_element_cache` — the old tree is dead, so the handle cache and listeners must not remain;
5. the VM itself is **not rebuilt** — it needs to re-run all the scripts of the spliced-back page, with globals starting over;
6. **mutation log** (`m_jsMutations`): a script's DOM writes (text/attribute/title, three kinds) each record a `(kind, id, name, value)`, a new value for the same key overwriting the old; when the new document is built, `replayJsMutations()` replays them by element id — so a page doing `document.getElementById('x').textContent = ...` followed by `document.write` still has its change after reparsing.

## 6.6 Element-Handle Liveness Management

A JS-side element handle is a raw `litehtml::element*`. A script may outlive a node (the node was detached by an `innerHTML` assignment or `removeChild`); there are two lines of defense:

- **`el_is_live` callback** (js_dom bridge): checks the liveness flag before restoring a handle from a JS wrapper each time;
- **`sys.ptr_sane`** (an optional porting hook, `ewok_ptr_in_heap` on EwokOS): before reading the liveness flag, does a **non-dereferencing** heap-pointer sanity check — blocking forged handles.

A subtree detached by `removeChild`/`innerHTML` is not deleted immediately but **parked into `m_jsDetached`** (a script may still hold handles or listeners pointing at them), freed uniformly by `jsFreeDetachedNodes()` when the page is destroyed; if a script re-inserts a node into the tree (`appendChild`), it is first removed from the parking list (`js_unpark`).

The `innerHTML` setter approximates with **tag stripping** (litehtml has no HTML-fragment parser): it keeps only the text content, replacing it with a single `el_text` child node.

## 6.7 JS-Side Capability Quick Reference

- **Language**: all of ES5 + a large set of ES6+ features (class, arrow functions, let/const, template strings, Promise, Proxy, TypedArray, BigInt, RegExp… see the mario_js docs Ch. 11 for details);
- **DOM**: getElementById/querySelector(All)/getElementsBy*, createElement/TextNode, tree traversal and add/remove, textContent/innerText/innerHTML, attributes and classList, style read/write, offsetX/clientX/getBoundingClientRect, focus/scrollIntoView;
- **Event**: full capture→target→bubble propagation, `addEventListener` (capture/once), `dispatchEvent`, on* properties, inline on\* attributes compiled and run on demand;
- **BOM**: location/history/navigator/screen/performance, localStorage/sessionStorage, document.cookie, synchronous XHR and fetch, atob/btoa, crypto.getRandomValues;
- **Canvas**: full CanvasRenderingContext2D + Path2D + ImageData (Ch. 8).

Known gaps (the "Known gaps" header of js_web.h): `postMessage` is a no-op; `globalThis` aliases `window` rather than the VM global; `performance.mark/measure` store no timeline; `crypto.getRandomValues` is seeded from the monotonic clock (good only for ids); XHR is always synchronous.
