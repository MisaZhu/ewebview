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

/* Layout debounce / force caps (see applyPendingLayoutUpdates). */
static const uint32_t kLayoutDebounceMs    = 30;
static const uint32_t kLayoutMaxWaitMs     = 200;
static const uint32_t kLayoutBehindStyleMs = 500;
/* Cap on how long a pending master-style walk may wait for the remaining
 * <link> sheets (m_pendingCss) while subresources keep re-dirtying layout:
 * past it the walk starts anyway (new sheets restart it from scratch). */
static const uint32_t kStyleMaxWaitMs      = 1500;
/* Wall-clock budget for ONE chunk of a master-style pass, so the engine loop
 * returns to drain commands (STOP/NAVIGATE) and render frames even mid-walk. */
static const uint64_t kStyleBudgetIdleMs   = 25;

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
    EWebCmd() : kind(ECMD_NAVIGATE), x(0), y(0), b(false) { memset(&ev, 0, sizeof(ev)); }
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
    void taskLoop();                                 /* download worker body */
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
    void clampScrollLocked(int docWidth, int docHeight);
    void postScrollClamp();
    void requestBuildAbort();

    bool hasSeenCSS(const std::string& url) const;
    void rememberCSS(const std::string& url);
    void forgetCSS(const std::string& url);

    bool addTask(const EWebTask& task);
    void removeTask(const std::string& url);
    bool getTask(EWebTask& task);
    std::string taskPageUrl();
    void setTaskPageUrl(const std::string& url);
    bool loadHtmlTask(const std::string& url);
    bool loadCSSTask(const std::string& url);
    bool loadImageTask(const std::string& url);
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
    void runPageScripts();
    bool runNextPageScript();
    void jsProgressiveFlush(bool force);
    bool applyJsWriteBuffer();
    void jsVmEnter();
    bool jsVmExit();
    void jsOnVmStep(struct st_vm* vm);
    static void jsVmStepHook(struct st_vm* vm, void* data);
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
    /* Dispatch the DOM mouse events for one pointer gesture (state/button are
     * the public EWEB_MOUSE_x / EWEB_BUTTON_x values). Returns false when a
     * listener cancelled it. */
    bool jsDispatchMouseEvent(int mouseState, int button, int cx, int cy);
    /* Follow an <a href> under a left-button release. cx/cy are client coords. */
    void handleAnchorClick(int cx, int cy);
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
    static void* jsElParent(void* ctx, void* el);
    static int   jsElChildCount(void* ctx, void* el);
    static void* jsElChild(void* ctx, void* el, int idx);
    static bool  jsElIsTag(void* ctx, void* el);
    static bool  jsElIsLive(void* ctx, void* el);
    static bool  jsElAppendChild(void* ctx, void* parent, void* child);
    static bool  jsElInsertBefore(void* ctx, void* parent, void* child, void* ref);
    static bool  jsElRemoveChild(void* ctx, void* parent, void* child);
    static void  jsElRemoveAttr(void* ctx, void* el, const char* name);
    static void  jsElGetRect(void* ctx, void* el, int* x, int* y, int* w, int* h);
    static char* jsElGetStyle(void* ctx, void* el, const char* prop);
    static void  jsElFocus(void* ctx, void* el);
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

    /* Download-worker lifecycle. m_task_running says a worker thread is alive
     * (set by addTask, cleared by the worker as it exits); m_task_ended is the
     * shutdown flag. Both cross threads, hence volatile. */
    volatile bool               m_task_running;
    volatile bool               m_task_ended;
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

    /* ---- JavaScript (mario VM) state ---- */
    struct st_vm*               m_jsVm;
    std::vector<std::string>    m_jsScripts;
    bool                        m_jsHasInlineHandlers;
    bool                        m_jsEnabled;
    int                         m_jsReparseCount;
    bool                        m_jsRunBeforePaint;
    size_t                      m_jsNextScript;
    bool                        m_jsPostSwapRun;
    bool                        m_jsProgressiveActive;
    uint64_t                    m_jsLastFlushAt;
    uint32_t                    m_jsFlushCostMs;
    bool                        m_jsInScript;
    uint64_t                    m_jsEnterAt;
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
    /* Nodes removeChild() detached but could not free (JS may still hold
     * handles/listeners on them); freed by jsFreeDetachedNodes(). */
    std::vector<litehtml::element*> m_jsDetached;

    /* ---- Canvas 2D registry: one EWebCanvas per live <canvas> id ---- */
    std::vector<EWebCanvas*>    m_jsCanvases;
};

} /* namespace eweb */
