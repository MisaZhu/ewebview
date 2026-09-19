/* EWebInternal.h - shared internals of the ewebview core.
 *
 * The core is split across three translation units that all need the engine
 * type, exactly like the widget++ original (WidgetWebview.cc /
 * WidgetWebviewJs.cc / WidgetWebviewCanvas.cc):
 *
 *   ewebview.cc        - the public C API (ewebview.h), the engine thread and
 *                        its command/event queues, the download worker, the
 *                        build state machine, the frame pool, rendering.
 *   EWebJs.cc          - the mario VM lifecycle and the DOM/event/web bridge
 *                        callbacks (js_dom.h / js_event.h / js_web.h).
 *   EWebCanvasGlue.cc  - the Canvas 2D bridge trampolines (js_canvas.h), the
 *                        per-page EWebCanvas registry and the final composite.
 *
 * Nothing here names EwokOS: platform access goes through eweb_port_t, the
 * drawing surface is an opaque eweb_surface_t, and input arrives as the
 * public EWEB_MOUSE_x / EWEB_BUTTON_x enums. All methods are ENGINE-THREAD ONLY
 * unless a comment says otherwise; the embedder's UI thread reaches the
 * engine only through the post*()/tick()/releaseFrame() entry points.
 */

#pragma once

#include <ewebview.h>       /* public C API (event enums, listener, task kinds) */
#include <ewebview_port.h>  /* the porting HAL tables                            */

#include "EWebContainer.h"
#include "EWebCanvas.h"

#include <litehtml.h>
#include <litehtml/context.h>

#include <pthread.h>
#include <stdint.h>
#include <string>
#include <string.h>   /* memset: EWebCmd ctor */
#include <vector>
#include <deque>
#include <unordered_map>

/* mario JavaScript VM handle; the full definition lives in <mario/mario.h>
 * (a C header) and is only pulled in by the .cc files that touch the VM. */
struct st_vm;
struct st_var;   /* mario var_t; full definition lives in <mario/mario.h> */

namespace eweb {

/* ------------------------------------------------------------------ */
/* Shared tuning constants (ported 1:1 from WidgetWebview.cc)          */
/* ------------------------------------------------------------------ */

/* document.write() reparse ceiling. Each drained write buffer splices into
 * the pending HTML and restarts the build once; this caps the restarts so a
 * script that writes in a loop cannot spin forever. */
static const int      kJsMaxReparse      = 8;

/* Progressive-flush pacing for the post-swap script run: the gap between two
 * mid-script repaints adapts to the measured cost of the previous flush
 * (cost x 3), clamped into this window. */
static const uint32_t kJsFlushMinGapMs   = 200;
static const uint32_t kJsFlushMaxGapMs   = 1500;

/* Hard wall-clock budget for ONE VM run (a page script, a timer callback or
 * an event handler). Past it the step hook terminates the VM; after
 * kJsRunAbortMax violations the page's JS is dropped until the next
 * navigation, so no script - broken or hostile - can pin the engine. */
static const uint32_t kJsRunBudgetMs     = 10000;
static const int      kJsRunAbortMax     = 3;
/* Per-run budget OUTSIDE the pre-paint phase (post-swap scripts, timer
 * callbacks, event handlers): those run against a live page whose input queue
 * the engine thread cannot drain while a VM run is in flight, so a runaway
 * body freezes clicks and scrolls for the whole budget. The pre-paint phase
 * keeps kJsRunBudgetMs because its own wall clock (kJsPrePaintBudgetMs) cuts
 * the run long before this one would. */
static const uint32_t kJsRunBudgetLiveMs = 3000;

/* Per-run budget while the viewport still shows ONLY the server-side skeleton
 * placeholder (client-rendered shells such as taobao.com). The bundle that
 * replaces the skeleton (the React/ice.js app entry: webpack bootstrap plus the
 * first synchronous render pass) is one single top-level body that legitimately
 * needs several seconds on this VM; cutting it at kJsRunBudgetLiveMs discards
 * the whole boot and the skeleton stays forever. Once real content paints the
 * ordinary short live budget applies again so input stays responsive. */
static const uint32_t kJsRunBudgetSkeletonMs = 15000;

/* Re-evaluation interval for jsSkeletonProbeCached(): short enough that the
 * budget drops back to the live one promptly once real content paints, long
 * enough that a timer-driven page does not re-walk the DOM every tick. */
static const uint32_t kJsSkeletonProbeTtlMs = 500;

/* Wall-clock budget for the WHOLE pre-paint script phase (the document.write()
 * pages that must run their scripts before the first paint). Past it the build
 * stops running scripts, splices whatever was written and paints; the scripts
 * that did not run move to the post-swap phase, where they execute one per
 * engine step against the visible page. Without this a page carrying a slow or
 * hanging script shows nothing for a run budget per script - minutes on an
 * ad-heavy portal - because nothing paints before the phase completes. */
static const uint32_t kJsPrePaintBudgetMs = 4000;

/* Wall-clock budget for the WHOLE post-swap script phase (BUILD_RUN_JS, one
 * script per engine step against the visible page). Each script already caps
 * at kJsRunBudgetLiveMs, but an ad-heavy portal carries dozens of them, so
 * without a phase cap the engine spends run-budget after run-budget on ad SDKs
 * and never returns to serving input, a queued navigation, or the blank-SPA
 * notice decision - the page looks frozen for tens of seconds. Past this the
 * phase stops STARTING new scripts (one already in flight still unwinds under
 * its own per-run budget), fires the load events and goes idle, exactly like
 * the pre-paint budget paints first and defers the rest. */
static const uint32_t kJsPostSwapBudgetMs = 8000;

/* Post-swap phase budget while the viewport still shows ONLY a server-side
 * skeleton placeholder (client-rendered shells such as taobao.com). Dropping
 * the tail of the script queue in that state guarantees a useless permanent
 * skeleton, because the bundle that replaces it (React app entry + its mtop
 * data call) sits behind the heavy telemetry/nav SDKs that consume the normal
 * budget. While nothing real is on screen we therefore keep starting scripts
 * up to this much larger cap; the moment real content paints the ordinary
 * kJsPostSwapBudgetMs applies again so ad-heavy portals still go idle fast. */
static const uint32_t kJsPostSwapSkeletonBudgetMs = 45000;

/* Expand a CDN combo URL ("https://host/path/??a.js,b.js,c.js") into one URL
 * per component. A combo downloads as ONE script body, so a watchdog cut on a
 * hanging middle component (taobao's jstracker telemetry spins forever between
 * lib-mtop and lib-env in the same bundle) discards every component after it
 * and starves the page's data path. Queueing the components as separate slots
 * keeps document order while giving each its own run budget. Non-combo URLs
 * pass through untouched. */
inline std::vector<std::string> ewebSplitComboUrl(const std::string& url) {
    std::vector<std::string> out;
    size_t q = url.find("/??");
    if(q == std::string::npos) { out.push_back(url); return out; }
    std::string base = url.substr(0, q + 1);   /* through the trailing '/' */
    std::string rest = url.substr(q + 3);
    size_t start = 0;
    while(start <= rest.size()) {
        size_t comma = rest.find(',', start);
        std::string part = (comma == std::string::npos)
            ? rest.substr(start) : rest.substr(start, comma - start);
        if(!part.empty()) {
            if(part.compare(0, 7, "http://") == 0 || part.compare(0, 8, "https://") == 0)
                out.push_back(part);
            else
                out.push_back(base + part);
        }
        if(comma == std::string::npos) break;
        start = comma + 1;
    }
    if(out.empty()) out.push_back(url);
    return out;
}

/* Layout debounce / force caps (see applyPendingLayoutUpdates). */
static const uint32_t kLayoutDebounceMs    = 30;
static const uint32_t kLayoutMaxWaitMs     = 200;
static const uint32_t kLayoutBehindStyleMs = 500;
/* Cap on how long a pending master-style walk may wait for the remaining
 * <link> sheets (m_pendingCss) while subresources keep re-dirtying layout:
 * past it the walk starts anyway (new sheets restart it from scratch). */
static const uint32_t kStyleMaxWaitMs      = 1500;
/* How long BUILD_RENDER_DOC holds the first paint while the <link> sheets
 * queued during the parse (m_pendingCss) are still in flight: the previous
 * page and the progress overlay stay on screen instead of an unstyled
 * skeleton frame (default bullets/link colours, every normally-hidden
 * responsive duplicate visible) that would then restyle live for seconds.
 * Past the cap the paint proceeds with whatever landed - a stuck fetch must
 * not black the view out - and late sheets restyle the visible page. */
static const uint32_t kFirstPaintCssWaitMs = 6000;
/* Wall-clock budget for ONE chunk of a master-style pass, so the engine loop
 * returns to drain commands (STOP/NAVIGATE) and render frames even mid-walk. */
static const uint64_t kStyleBudgetIdleMs   = 25;

/* Download worker pool. The pool starts empty; addTask() wakes a parked
 * worker or spawns a new one (up to kTaskPoolMax) only when every live
 * worker is busy. A worker idle for kTaskIdleExitMs tears itself down, so
 * the pool shrinks back to zero once a page's fetches drain. */
static const int      kTaskPoolMax         = 8;
static const uint32_t kTaskIdleExitMs      = 4000;

/* ------------------------------------------------------------------ */
/* Download worker payload types                                       */
/* ------------------------------------------------------------------ */

/* A queued sub-resource fetch. type is one of the public EWEB_TASK_*. */
struct EWebTask {
    std::string url;
    int         type;
    bool        loading;
};

/* A completed fetch. Image tasks carry a bitmap decoded on the worker thread
 * (pure heap work), handed to the engine for an O(1) mount. Ownership stays
 * with the queue entry until consumed: a requeued result keeps the pointer, a
 * dropped one (engineDropPendingWork on a STOP/NAVIGATE) must free it. */
struct EWebResult {
    std::string      url;
    int              type;
    bool             ok;
    std::string      content;
    eweb_surface_t*  image;
};

/* ------------------------------------------------------------------ */
/* Engine <-> UI channels                                              */
/* ------------------------------------------------------------------ */

/* UI -> engine command kinds. The four termination triggers (STOP, NAVIGATE,
 * RELOAD, SHUTDOWN) all land here so the engine - which owns the documents,
 * the VM and the surfaces - performs the cleanup in its own context. */
enum EWebCmdKind {
    ECMD_NAVIGATE = 0,
    ECMD_STOP,
    ECMD_RELOAD,
    ECMD_RESIZE,
    ECMD_SCROLL,
    ECMD_INPUT,
    ECMD_KEY,
    ECMD_SET_JS,
    ECMD_SHUTDOWN,
};

struct EWebCmd {
    int          kind;   /* EWebCmdKind                                */
    std::string  url;    /* NAVIGATE target                            */
    int          x;      /* RESIZE w / SCROLL x                        */
    int          y;      /* RESIZE h / SCROLL y                        */
    bool         b;      /* SET_JS enabled                             */
    eweb_event_t ev;     /* INPUT: a copy of the pointer event         */
    eweb_key_event_t kev;/* KEY: a copy of the keyboard event          */
    EWebCmd() : kind(ECMD_NAVIGATE), x(0), y(0), b(false) {
        memset(&ev, 0, sizeof(ev));
        memset(&kev, 0, sizeof(kev));
    }
};

/* Engine -> UI event kinds, drained by ewebview_tick() which fires the
 * matching eweb_listener_t hook for each. */
enum EWebUiEventKind {
    EUET_FRAME = 0,      /* a rendered frame is ready to adopt (m_pendingFrame) */
    EUET_SCROLL_CLAMP,   /* engine-authoritative scroll offset + doc geometry   */
    EUET_URL,            /* visible page url changed                            */
    EUET_BUILD_STATUS,   /* status text + progress + overlay flag (in w)        */
    EUET_TITLE,          /* document.title changed                              */
    EUET_DIALOG,         /* alert/confirm/prompt text (non-blocking)            */
    EUET_TASK_START,
    EUET_TASK_END,
    EUET_TASK_FAILED,
    EUET_TASKS_END,
};

struct EWebUiEvent {
    int         kind;
    std::string text;      /* URL/TITLE/DIALOG/BUILD_STATUS payload */
    int         x;         /* FRAME render scroll X / SCROLL_CLAMP x */
    int         y;         /* FRAME render scroll Y / SCROLL_CLAMP y */
    int         w;         /* FRAME doc width  / BUILD_STATUS overlay flag */
    int         h;         /* FRAME doc height / SCROLL_CLAMP doc height   */
    int         progress;  /* BUILD_STATUS progress                        */
    EWebTask    task;      /* TASK_* payload                               */
    EWebUiEvent() : kind(EUET_FRAME), x(0), y(0), w(0), h(0), progress(0) {}
};

/* ------------------------------------------------------------------ */
/* The engine                                                          */
/* ------------------------------------------------------------------ */

class EWebEngine : public EWebContainerHost {
public:
    explicit EWebEngine(const eweb_port_t* port);
    ~EWebEngine();

    /* ---- embedder-facing entry points (UI thread unless noted) ---- */
    void        setListener(const eweb_listener_t* listener);
    void        tick();                              /* drain engine->UI queue */
    void        releaseFrame(eweb_surface_t* frame); /* return an adopted frame */
    void        setViewport(int w, int h);           /* posts ECMD_RESIZE  */
    void        setDefaultCSS(const char* url);      /* direct setter      */
    void        setJSEnabled(bool enabled);          /* posts ECMD_SET_JS  */
    void        load(const char* url);               /* posts ECMD_NAVIGATE */
    void        stop();                              /* posts ECMD_STOP    */
    void        reload();                            /* posts ECMD_RELOAD  */
    void        scrollTo(int x, int y);              /* posts ECMD_SCROLL  */
    void        postInput(const eweb_event_t& ev);   /* posts ECMD_INPUT   */
    void        postKey(const eweb_key_event_t& ev); /* posts ECMD_KEY     */
    /* UI-thread mirror of the visible page's URL (refreshed by EUET_URL). */
    const char* currentUrlUi() const { return m_uiCurrentUrl.c_str(); }

    /* ---- EWebContainerHost (engine thread, called from litehtml) ---- */
    virtual bool queueImageTask(const std::string& url) override;
    virtual void loadCSS(const std::string& url) override;
    virtual void setCSSMedia(const std::string& url, const std::string& media) override;
    virtual void queueNavigation(const std::string& url) override;
    virtual bool buildAbortRequested() const override {
        return m_buildAbort && m_buildPhase != BUILD_IDLE;
    }

    /* ---- small port accessors ----------------------------------------
     * ticMs: the monotonic millisecond clock (REQUIRED table).
     * portSleepMs: clock.sleep_ms, with a pthread timed-wait fallback when the
     * port leaves it NULL (used by the destructor's worker-reap spin).
     * surfaceW/H: pixel size of a surface (engine thread only - the gfx table
     * is engine-thread per the HAL contract). */
    uint64_t ticMs() const { return m_port.clock.tic_ms(m_port.clock.ud); }
    void     portSleepMs(uint32_t ms);
    int surfaceW(eweb_surface_t* s) const {
        int w = 0, h = 0;
        if (m_port.gfx.surface_dims) m_port.gfx.surface_dims(m_port.gfx.ud, s, &w, &h);
        return w;
    }
    int surfaceH(eweb_surface_t* s) const {
        int w = 0, h = 0;
        if (m_port.gfx.surface_dims) m_port.gfx.surface_dims(m_port.gfx.ud, s, &w, &h);
        return h;
    }

    /* ---- engine thread core (ewebview.cc) ---- */
    void engineLoop();
    void engineStart();
    void engineStop();
    void taskLoop();                                 /* one pool worker's body */
    /* Park on m_taskCond (m_taskMutex held on entry AND exit) until a task is
     * claimable, shutdown is raised or timeout_ms elapses; true = a task is
     * waiting. */
    bool taskWaitLocked(uint32_t timeout_ms);
    void postCommand(const EWebCmd& cmd);            /* UI -> engine (any thread) */
    void postUiEvent(const EWebUiEvent& ev);         /* engine/worker -> UI */
    bool engineHandleCommand(const EWebCmd& cmd);    /* false on ECMD_SHUTDOWN */
    void engineNavigate(const std::string& url);
    void engineAbortBuild();
    void engineDropPendingWork();
    void engineTeardown();
    void engineRenderFrame(bool full);
    void engineEnsureFramePool();
    void engineFreeFramePool();

    void cleanupBuildResources();
    void setBuildStatus(const std::string& status, int progress);
    void advanceBuildStep();
    bool applyPendingLayoutUpdates();
    void markLayoutDirty(bool build);
    void markContentDirty();
    void drawPageToCacheLocked(int stripY, int stripH);
    void decideModuleNotice();
    void decideCsrNotice();
    bool pageShowsSkeletonPlaceholder() const;
    /* TTL-cached pageShowsSkeletonPlaceholder(): the probe is a full DOM walk,
     * and jsVmEnter() consults it on EVERY VM run (timer ticks included), so an
     * uncached call would re-walk the whole tree per tick on non-skeleton
     * pages. Re-evaluate at most once per kJsSkeletonProbeTtlMs. */
    bool jsSkeletonProbeCached();
    void drawModuleNotice(eweb_surface_t* cache, int cacheW, int cacheH);
    void clampScrollLocked(int docWidth, int docHeight);
    void postScrollClamp();
    void requestBuildAbort();

    bool hasSeenCSS(const std::string& url) const;
    void rememberCSS(const std::string& url);
    void forgetCSS(const std::string& url);

    bool addTask(const EWebTask& task);
    void removeTask(const std::string& url);
    bool getTask(EWebTask& task);
    bool getTaskLocked(EWebTask& task);   /* m_taskMutex held */
    std::string taskPageUrl();
    void setTaskPageUrl(const std::string& url);
    bool loadHtmlTask(const std::string& url);
    bool loadCSSTask(const std::string& url);
    bool loadImageTask(const std::string& url);
    bool loadScriptTask(const std::string& url);
    void pushResult(const EWebResult& result);
    bool getResult(EWebResult& result);
    bool processResults();
    bool loadCSSContent(const std::string& url, const std::string& content);
    bool loadHtmlContent(const std::string& content);
    bool loadImageContent(const std::string& url, uint8_t* data, int sz);
    bool mountDecodedImage(const std::string& url, eweb_surface_t* img);

    /* ---- JavaScript (mario VM) integration (EWebJs.cc) ----
     * Everything below runs on the engine thread, which exclusively owns the
     * VM and the documents. */
    void initJsVm();
    void resetJsVm();
    bool runPageScripts();
    bool runNextPageScript();
    /* A <script> spliced into the tree by a DOM mutation (appendChild /
     * insertBefore / replaceChild) must fetch+run like a parser-inserted one:
     * append an ordered slot (external src fetched async, inline body ready)
     * and re-arm BUILD_RUN_JS if the page had already gone idle. */
    void jsDynamicScriptInserted(void* script_el);
    void jsProgressiveFlush(bool force);
    bool applyJsWriteBuffer();
    void jsDropWriteBuffer();
    bool jsIsRunaway(const std::string& src) const;
    void jsVmEnter();
    bool jsVmExit();
    void jsOnVmStep(struct st_vm* vm);
    static void jsVmStepHook(struct st_vm* vm, void* data);
    /* Await spin tick (mario vm->on_await_pending): pump the DOM timer /
     * message-queue so a promise awaited by __await() settles instead of
     * degrading to undefined. See EWebJs.cc. */
    static int  jsAwaitPendingTick(struct st_vm* vm, struct st_var* promise);
    void registerEventNatives(struct st_vm* vm);
    void registerWebNatives(struct st_vm* vm);

    /* The document a script operates on: the build doc while a build is in
     * flight, the visible doc afterwards. BUILD_SWAP_DOC hands over the SAME
     * document object, so element handles stay valid for the page's life. */
    litehtml::document* jsActiveDoc() const { return (m_buildDoc != nullptr) ? m_buildDoc : m_doc; }
    bool jsActiveIsBuild() const { return m_buildDoc != nullptr; }
    /* URL of the document a script is talking to (document.cookie scoping). */
    const std::string& jsDocumentUrl() const {
        return (!m_buildHtmlUrl.empty()) ? m_buildHtmlUrl : m_currentHtmlUrl;
    }
    void jsMarkLayoutDirty() { markLayoutDirty(jsActiveIsBuild()); jsProgressiveFlush(false); }

    void jsFireLoadEvents();
    void jsFireResizeEvent();
    void jsFireScrollEvent();
    /* Poll setInterval/setTimeout callbacks (engine loop step 7). Brackets the
     * poll with jsVmEnter/jsVmExit and returns how many timers fired (0 when
     * the VM is off/disabled/page-disabled). */
    int  jsPollTimers();
    /* TEMP DIAGNOSTIC (EWEB_DOMDBG): print #ice-container subtree size. */
    void jsDomMountDiag();
    /* Fire "load"/"error" on the dynamic <script> element of slot i so
     * webpack's d.l chunk loader settles (see m_jsScriptEls). */
    void jsFireScriptElEvent(size_t i, const char* type);
    /* Dispatch the DOM mouse events for one pointer gesture (state/button are
     * the public EWEB_MOUSE_x / EWEB_BUTTON_x values). Returns false when a
     * listener cancelled it. */
    bool jsDispatchMouseEvent(int mouseState, int button, int cx, int cy);
    /* Fire a non-cancelable DOM event (focus/blur/focusin/focusout/...) at an
     * element. No-op without a VM. */
    void jsDispatchSimpleEvent(litehtml::element* el, const char* type, bool bubbles);
    /* Fire a cancelable DOM event (e.g. "submit") at an element; returns false
     * when a listener called preventDefault(). True (no-op) without a VM. */
    bool jsDispatchCancelableEvent(litehtml::element* el, const char* type, bool bubbles);
    /* Follow an <a href> under a left-button release. cx/cy are client coords. */
    void handleAnchorClick(int cx, int cy);

    /* ---- keyboard + focus + form-control interaction (engine thread) ----
     * One keyboard gesture: dispatch the DOM key events to the focused element
     * and, unless cancelled, run the default action (text editing, control
     * activation, focus traversal). */
    void handleKeyEvent(const eweb_key_event_t& kev);
    /* The engine's built-in response to a key that was not cancelled: text
     * editing on the focused field, Space/Enter activation of a focused
     * control, Tab focus traversal, arrow-key handling in an open <select>.
     * `domKey`/`mods` are the already-mapped DOM key string and JS_EVENT_MOD_*
     * bits computed by handleKeyEvent. */
    void handleKeyDefault(const eweb_key_event_t& kev, const char* domKey, unsigned mods);
    /* Move focus to `el` (NULL clears it), firing blur/focusout on the old and
     * focus/focusin on the new element. Repaints when the caret changes. */
    void setFocus(litehtml::element* el);
    void clearFocus();
    /* Toggle a runtime pseudo-class (e.g. "hover", "focus") on `el` and every
     * ancestor up to the root, then re-run the CSS cascade so :hover/:focus/
     * :focus-within selectors that just started or stopped matching take
     * effect. Returns true when any element's pseudo-class list actually
     * changed. Safe to call with el=NULL. */
    bool applyPseudoClassChain(litehtml::element* el, const char* name, bool add);
    /* Focus traversal across focusable elements (form widgets + <a href>) in
     * document order; reverse walks backwards (Shift+Tab). */
    void focusNext(bool reverse);
    /* Map a hit element to the form control it belongs to (walks up ancestors),
     * or NULL. The pointer is an eweb_el_input* (see eweb_el_input.h). */
    void* widgetAt(litehtml::element* el) const;
    /* Nearest focusable element at/above `el` (a form widget or an <a href>). */
    litehtml::element* focusableAt(litehtml::element* el) const;
    /* The element under the given client coords (viewport-relative), or NULL.
     * Same hit test jsDispatchMouseEvent uses. */
    litehtml::element* hitElementAt(int cx, int cy);
    /* Run a form control's click default action (toggle/activate/open/caret). */
    void activateWidget(void* widget, int localX, int localY);
    /* Submit the <form> enclosing `field` (GET -> action?query navigation). */
    void submitForm(litehtml::element* field);
    /* Draw an open <select>'s dropdown overlay onto the frame (after the page). */
    void drawSelectPopup(eweb_surface_t* cache);
    /* Compute the open <select>'s dropdown overlay rect (logical/cache coords)
     * plus its row height and visible row count; false when nothing is open.
     * Shared by drawSelectPopup() and the popup mouse hit-test so they agree. */
    bool selectPopupRect(litehtml::position& out, int& rowH, int& visibleRows);
    /* Uncheck the same-name radio siblings of `widget` across the active doc. */
    void clearRadioSiblings(void* widget);
    /* Fire a bubbling, non-cancelable input/change event at a form control. */
    void fireWidgetEvent(litehtml::element* el, const char* type);
    /* Keyboard page scroll (line/page step): adopts the offset, repaints, fires
     * the page's scroll handlers and republishes it to the embedder. */
    void scrollByKey(int dx, int dy);
    void jsRunPendingNavigation();
    void jsInvalidateHandles();
    void jsFreeDetachedNodes();

    /* DOM bridge callbacks (js_dom_callbacks_t signatures). `ctx` is always
     * the owning EWebEngine; element handles are litehtml::element*. */
    static void  jsAlert(void* ctx, const char* msg);
    static void  jsDocumentWrite(void* ctx, const char* html);
    static char* jsGetTitle(void* ctx);
    static void  jsSetTitle(void* ctx, const char* title);
    static char* jsGetUrl(void* ctx);
    static void* jsGetElementById(void* ctx, const char* id);
    static char* jsElGetText(void* ctx, void* el);
    static void  jsElSetText(void* ctx, void* el, const char* text);
    static char* jsElGetHtml(void* ctx, void* el);
    static void  jsElSetHtml(void* ctx, void* el, const char* html);
    static char* jsElGetAttr(void* ctx, void* el, const char* name);
    static void  jsElSetAttr(void* ctx, void* el, const char* name, const char* value);
    static char* jsElGetTag(void* ctx, void* el);
    static void* jsGetRoot(void* ctx);
    static void* jsGetBody(void* ctx);
    static void* jsGetHead(void* ctx);
    static int   jsQueryAll(void* ctx, void* root, const char* selector,
                            int skip, void** out, int max);
    static void* jsCreateElement(void* ctx, const char* tag);
    static void* jsCreateTextNode(void* ctx, const char* text);
    static void* jsCreateComment(void* ctx, const char* text);
    static void* jsElParent(void* ctx, void* el);
    static int   jsElChildCount(void* ctx, void* el);
    static void* jsElChild(void* ctx, void* el, int idx);
    static bool  jsElIsTag(void* ctx, void* el);
    static bool  jsElIsComment(void* ctx, void* el);
    static bool  jsElIsLive(void* ctx, void* el);
    static bool  jsElAppendChild(void* ctx, void* parent, void* child);
    static bool  jsElInsertBefore(void* ctx, void* parent, void* child, void* ref);
    static void* jsElCloneNode(void* ctx, void* el, int deep);
    static bool  jsElRemoveChild(void* ctx, void* parent, void* child);
    static void  jsElRemoveAttr(void* ctx, void* el, const char* name);
    static void  jsElGetRect(void* ctx, void* el, int* x, int* y, int* w, int* h);
    static char* jsElGetStyle(void* ctx, void* el, const char* prop);
    static void  jsElFocus(void* ctx, void* el);
    static void  jsElBlur(void* ctx, void* el);
    static void* jsGetActiveElement(void* ctx);
    /* document.currentScript backend: the <script> element whose body is
     * executing right now (nullptr outside a script run). */
    static void* jsGetCurrentScript(void* ctx);
    void*        jsCurrentScriptEl();
    /* Stand-in <script src> element for a stripped static script tag: searches
     * the live tree for a <script> carrying this src, else materialises one in
     * <head> and returns it (nullptr if the doc/host is gone). Pre-materialising
     * every queued external script makes document.getElementsByTagName("script")
     * and document.scripts see real attached nodes - SDKs that locate themselves
     * via the script list otherwise get an empty list and dereference undefined
     * (taobao baxia: `ref.parentNode.insertBefore(...)` -> "can not find
     * function 'insertBefore'"). */
    void*        jsScriptStandInEl(const std::string& url);
    void         jsMaterializeScriptStandIns();
    /* selectionStart/End + setSelectionRange()/select() backends: codepoint
     * offsets into the focused text control's live value. */
    static bool  jsElGetSel(void* ctx, void* el, int* s, int* e);
    static void  jsElSetSel(void* ctx, void* el, int s, int e);
    /* Element.dataset seed: packs the element's data-* attributes (keys
     * camelCased) as "key\x1fvalue\x1e" in a mario_malloc'd buffer. */
    static char* jsElGetDataset(void* ctx, void* el);
    static char* jsElAttrSnapshot(void* ctx, void* el);
    static void  jsElScrollIntoView(void* ctx, void* el);

    /* Web/BOM bridge callbacks (js_web_callbacks_t signatures). */
    static bool  jsWebConfirm(void* ctx, const char* message);
    static char* jsWebPrompt(void* ctx, const char* message, const char* def);
    static void  jsWebGetViewport(void* ctx, int* w, int* h);
    static void  jsWebGetScreen(void* ctx, int* w, int* h, int* depth);
    static void  jsWebGetScroll(void* ctx, int* x, int* y);
    static void  jsWebScrollTo(void* ctx, int x, int y);
    static void  jsWebNavigate(void* ctx, const char* url);
    static void  jsWebReload(void* ctx);
    static char* jsWebGetCookie(void* ctx);
    static void  jsWebSetCookie(void* ctx, const char* cookie);
    static char* jsWebStorageLoad(void* ctx, bool session);
    static void  jsWebStorageSave(void* ctx, bool session, const char* blob);
    /* Synchronous XHR/fetch backend: performs one blocking request on the
     * engine thread (direct port->net.request call, never the download task
     * queue). out_body/out_headers are mario_malloc'd; false = network
     * error. Redirects are followed here, cookies scoped per hop. */
    static bool  jsWebRequest(void* ctx, const char* method, const char* url,
                              const char* headers, const char* body,
                              int* status, char** out_body, char** out_headers);

    /* ---- Canvas 2D (EWebCanvasGlue.cc) ---- */
    void registerCanvasNatives(struct st_vm* vm);
    EWebCanvas* getOrCreateCanvas(const std::string& id, int w, int h);
    void freeCanvases();
    void compositeCanvases(eweb_surface_t* g, int ox, int oy);
    static void* jsCanvasCreate(void* ctx, const char* id, int w, int h);
    static void* jsCanvasBitmapFromElement(void* ctx, void* el);

    /* DOM-mutation journal: survives a document.write() reparse so the rebuilt
     * page keeps script-applied state. Cleared per page load. */
    struct JsMutation {
        int         kind;    /* 0 = text, 1 = attribute, 2 = document title */
        std::string id;      /* target element id (empty for kind 2)        */
        std::string name;    /* attribute name (kind 1 only)                */
        std::string value;
    };
    void recordJsMutation(int kind, const std::string& id,
                          const std::string& name, const std::string& value);
    void replayJsMutations();

    enum BuildPhase {
        BUILD_IDLE = 0,
        BUILD_PRELOAD_CSS,
        BUILD_CREATE_DOC,
        BUILD_RUN_JS,
        BUILD_RENDER_DOC,
        BUILD_SWAP_DOC,
        BUILD_FAILED,
    };

    /* The port, copied by value at create time. Containers and canvases keep
     * a pointer to this member, so it must never move: EWebEngine lives
     * inside struct ewebview and is addressed through it. */
    eweb_port_t                 m_port;

    std::string                 m_defaultCSSUrl;
    /* Authoritative URL of the visible page. ENGINE-THREAD ONLY. */
    std::string                 m_currentHtmlUrl;
    /* UI-thread mirror of m_currentHtmlUrl, refreshed from EUET_URL in
     * tick(); what ewebview_get_url() hands the embedder. */
    std::string                 m_uiCurrentUrl;

    EWebContainer*              m_container;
    litehtml::document::ptr     m_doc;
    EWebContainer*              m_pendingDeleteContainer;
    litehtml::document::ptr     m_pendingDeleteDoc;
    litehtml::context           m_browser_context;
    litehtml::context*          m_activeContext;
    BuildPhase                  m_buildPhase;
    std::string                 m_buildHtmlContent;
    std::string                 m_buildHtmlUrl;
    EWebContainer*              m_buildContainer;
    litehtml::document::ptr     m_buildDoc;
    litehtml::context           m_buildContext;
    litehtml::context*          m_buildTargetContext;

    /* Download worker-pool lifecycle (all guarded by m_taskMutex):
     * m_taskThreads counts the live pool workers (0 by default, grown by
     * addTask up to kTaskPoolMax, each worker decrements it on exit);
     * m_taskBusy counts the ones mid-fetch; m_taskCond wakes parked workers
     * (new task queued / shutdown). m_task_ended is the shutdown flag - read
     * outside the lock by the workers' wait loops, hence volatile. */
    int                         m_taskThreads;
    int                         m_taskBusy;
    volatile bool               m_task_ended;
    pthread_cond_t              m_taskCond;
    int                         m_clientWidth;
    int                         m_clientHeight;

    std::vector<EWebTask>       m_taskQueue;
    /* Document the download worker judges cookies against (guarded by
     * m_taskMutex; the worker reads it on every fetch). */
    std::string                 m_taskPageUrl;
    std::vector<std::string>    m_seenCssUrls;
    /* url -> <link media> attribute for in-flight stylesheet fetches. */
    std::unordered_map<std::string, std::string> m_cssMedia;
    pthread_mutex_t             m_cssMediaMutex;
    pthread_mutex_t             m_taskMutex;
    std::vector<EWebResult>     m_resultQueue;
    pthread_mutex_t             m_resultMutex;

    /* Engine thread + cross-thread channels. */
    pthread_t                   m_engineThread;
    bool                        m_engineStarted;
    volatile bool               m_engineStop;
    std::deque<EWebCmd>         m_cmdQueue;
    pthread_mutex_t             m_cmdMutex;
    pthread_cond_t              m_cmdCond;
    std::deque<EWebUiEvent>     m_uiQueue;
    pthread_mutex_t             m_uiMutex;
    /* The listener table (copied). Written by setListener, read by tick - both
     * UI-thread per the public contract; m_uiMutex guards it anyway. */
    eweb_listener_t             m_listener;

    /* Viewport frame pool (double-buffer). The engine renders into a surface
     * taken from m_freeFrames and posts it as m_pendingFrame; tick() hands it
     * to the listener (ownership transfer) and ewebview_release_frame()
     * returns it to the pool. Only pointer ownership crosses m_uiMutex. */
    std::vector<eweb_surface_t*> m_freeFrames;   /* guarded by m_uiMutex */
    eweb_surface_t*             m_pendingFrame;  /* engine-produced, awaiting adoption */
    int                         m_pendingFrameX;
    int                         m_pendingFrameY;
    int                         m_pendingDocW;
    int                         m_pendingDocH;

    /* The engine's authoritative scroll offset (the embedder keeps its own
     * live offset and blits frames shifted by the difference). */
    int                         m_engineScrollX;
    int                         m_engineScrollY;

    /* Dirty flags consumed by the engine loop so render stays draw-only. */
    bool                        m_needsStyleUpdate;
    uint64_t                    m_styleNeedSince;
    bool                        m_needsLayout;
    int                         m_pendingCss;
    /* Wall-clock anchor of the BUILD_RENDER_DOC render-blocking stylesheet
     * wait (kFirstPaintCssWaitMs); 0 = not waiting yet. */
    uint64_t                    m_firstPaintCssWaitSince;
    bool                        m_styleStepInFlight;
    /* Click-vs-drag bookkeeping (ECMD_INPUT). */
    int                         m_pressX;
    int                         m_pressY;
    bool                        m_pressValid;
    bool                        m_buildNeedsStyleUpdate;
    bool                        m_buildNeedsLayout;
    bool                        m_flushDeferredImages;
    bool                        m_defaultCssPrepared;
    bool                        m_defaultCssLoading;
    bool                        m_deferBuildStep;
    uint64_t                    m_layoutDirtyAt;
    uint64_t                    m_buildLayoutDirtyAt;
    uint64_t                    m_layoutDirtySince;
    uint64_t                    m_buildLayoutDirtySince;

    /* Engine render target: the pool surface currently being drawn into. */
    eweb_surface_t*             m_pageCache;
    int                         m_cacheScrollX;
    int                         m_cacheScrollY;
    bool                        m_cacheValid;
    bool                        m_contentDirty;
    uint64_t                    m_contentDirtySince;
    /* True when the visible document's animation timeline has at least one
     * running entry. Set by the engine-loop animation tick; included in the
     * busy check so the loop polls at 4ms instead of parking for 200ms while
     * an animation is in flight. */
    bool                        m_animActive;

    /* ---- JavaScript (mario VM) state ---- */
    struct st_vm*               m_jsVm;
    std::vector<std::string>    m_jsScripts;
    /* Parallel to m_jsScripts: the resolved absolute src of an external
     * <script src> ("" for an inline body) and whether its body is ready
     * (1) or still awaiting its EWEB_TASK_SCRIPT fetch (0). runNextPageScript
     * blocks on a pending slot so classic scripts run in document order. */
    std::vector<std::string>    m_jsScriptSrcs;
    std::vector<char>           m_jsScriptDone;
    /* Parallel to m_jsScripts (kept the same length at every mutation):
     * the litehtml element of a dynamically inserted <script>, nullptr for
     * parser-extracted slots. The ordered run fires "load"/"error" on it so
     * webpack's d.l chunk loader (script.onload = resolve) settles - without
     * it a chunk promise stays pending forever and e.g. Next.js RSC module
     * deps never resolve, stalling hydration (rokid.com's Header). Validate
     * with jsElIsLive before use: the node may have been removed. */
    std::vector<void*>          m_jsScriptEls;
    bool                        m_jsHasInlineHandlers;
    bool                        m_jsEnabled;
    int                         m_jsReparseCount;
    bool                        m_jsRunBeforePaint;
    size_t                      m_jsNextScript;
    bool                        m_jsInjectDone;   /* EWEB_INJECT_JS diagnostic ran once */
    uint64_t                    m_jsScriptWaitSince;
    bool                        m_jsPostSwapRun;
    /* Wall-clock anchor for the post-swap script phase (kJsPostSwapBudgetMs).
     * Set when the phase is armed - at BUILD_SWAP_DOC and again when a
     * dynamically injected <script> re-arms it - so a late lazy-load gets its
     * own budget instead of inheriting a stale one. */
    uint64_t                    m_jsPostSwapAt;
    /* ES-module awareness: extract_scripts flags a build whose scripts were
     * skipped as type="module". When such a page is on screen and its body
     * still holds no text a few seconds after the swap, the engine draws a
     * plain notice instead of leaving an unexplained blank viewport (the VM
     * cannot parse ES2020 module bundles, so SPAs render nothing). */
    bool                        m_jsBuildHasModules;
    bool                        m_jsPageHasModules;
    bool                        m_moduleNoticeDecided;
    bool                        m_showModuleNotice;
    /* Which explanation drawModuleNotice() paints: 1 = the page's content is
     * entirely ES-module generated, 2 = a client-rendered shell whose classic
     * scripts ran (or were dropped) without ever replacing the server-side
     * skeleton placeholder. 0 while no notice is armed. */
    int                         m_noticeKind;
    uint64_t                    m_swapAtMs;
    eweb_font_t*                m_noticeFont;
    bool                        m_jsProgressiveActive;
    uint64_t                    m_jsLastFlushAt;
    uint32_t                    m_jsFlushCostMs;
    bool                        m_jsInScript;
    uint64_t                    m_jsEnterAt;
    uint64_t                    m_jsRunDeadline;  /* wall clock at which the step hook cuts this run */
    uint64_t                    m_jsSkeletonProbeAt;   /* last jsSkeletonProbeCached() evaluation, 0 = never */
    bool                        m_jsSkeletonProbeVal;  /* cached result of the above */
    uint64_t                    m_jsPrePaintAt;   /* pre-paint phase start (0 = not in it) */
    bool                        m_jsAbortPrePaint; /* last termination was the pre-paint budget */
    bool                        m_jsPrePaintCut;  /* pre-paint phase ended on the budget, not on completion */
    const std::string*          m_jsCurScriptSrc; /* body in flight, for runaway bookkeeping */
    /* Resolved src of the <script> body in flight ("" for inline): backs
     * document.currentScript, which security SDKs use to insert their loader
     * next to themselves. */
    std::string                 m_jsCurScriptUrl;
    /* Stand-in element handed out as document.currentScript for a stripped
     * static <script src> (see jsCurrentScriptEl): the URL it was built for
     * and the cached node, so repeated reads see one stable element. */
    std::string                 m_jsCurScriptElUrl;
    void*                       m_jsCurScriptEl = nullptr;
    std::vector<std::string>    m_jsRunawaySrcs;  /* bodies the run budget terminated: never re-run */
    std::vector<std::string>    m_jsRequeuedSrcs; /* bodies already requeued once after a watchdog cut (see runNextPageScript) */
    uint32_t                    m_jsEnterGen;
    int                         m_jsAbortCount;
    bool                        m_jsPageDisabled;
    volatile bool               m_buildAbort;
    volatile uint32_t           m_buildAbortGen;
    std::vector<JsMutation>     m_jsMutations;

    /* ---- Web/BOM bridge state (engine thread only) ----
     * The storage blobs deliberately OUTLIVE the VM (rebuilt on every
     * navigation) so localStorage persists across page loads; they die with
     * the engine. Cookies live in the process-wide EWebCookieJar. */
    std::string                 m_jsLocalStorage;
    std::string                 m_jsSessionStorage;
    std::string                 m_jsPendingNav;
    bool                        m_jsScrollPending;
    void*                       m_jsHoverElement;  /* litehtml::element* */

    /* ---- keyboard / focus / form-control interaction (engine thread) ---- */
    void*                       m_focusElement;    /* focused litehtml::element* */
    void*                       m_openSelect;      /* eweb_el_input* with its dropdown open */
    bool                        m_rangeDragging;   /* a range slider is being dragged */
    bool                        m_textDragging;    /* a text control selection is being dragged */
    void*                       m_dragWidget;      /* eweb_el_input* the drag belongs to */
    /* Nodes removeChild() detached but could not free (JS may still hold
     * handles/listeners on them); freed by jsFreeDetachedNodes(). */
    std::vector<litehtml::element*> m_jsDetached;

    /* ---- Canvas 2D registry: one EWebCanvas per live <canvas> id ---- */
    std::vector<EWebCanvas*>    m_jsCanvases;
};

} /* namespace eweb */
