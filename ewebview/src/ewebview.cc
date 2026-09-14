/* ewebview.cc - the public C API and the engine core of the ewebview library.
 *
 * This translation unit owns:
 *   - the extern "C" API from <ewebview.h> (create/destroy/load/tick/...),
 *   - the engine thread and its command/event queues,
 *   - the on-demand download worker thread,
 *   - the page build state machine (BUILD_*),
 *   - the viewport frame pool and rasterization.
 *
 * The mario JS VM lifecycle and the DOM/event/web bridges live in EWebJs.cc,
 * the Canvas 2D glue in EWebCanvasGlue.cc, and the litehtml
 * document_container in EWebContainer.cc. Nothing here names EwokOS:
 * platform access goes through the eweb_port_t tables copied into the engine
 * at create time (see <ewebview_port.h>).
 *
 * Threading model (mirrors the original WidgetWebview):
 *   - the ENGINE thread exclusively owns the documents, the containers, the
 *     litehtml contexts, the mario VM and every surface it draws into;
 *   - the UI thread only signals (postCommand), drains events (tick) and
 *     returns adopted frames (releaseFrame);
 *   - the download worker touches only the mutex-guarded task/result queues.
 */

#include <ewebview.h>

#include "EWebInternal.h"
#include "EWebLog.h"
#include "eweb_el_input.h"

#include <pthread.h>
#include <sys/time.h>   /* gettimeofday: CLOCK_REALTIME abstime for the engine park */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace eweb {

/* ------------------------------------------------------------------ */
/* <script> extraction (ported 1:1 from WidgetWebview.cc)              */
/* ------------------------------------------------------------------ */

/* Scan `html` for <script> blocks. The returned string is the page with every
 * <script>...</script> removed (litehtml has no JS engine, and el_script would
 * otherwise render nothing but keep the raw text as a child). Inline script
 * bodies are appended to `scripts` in document order so the caller can feed
 * them to the mario VM after the DOM is built.
 *
 * Only inline scripts are collected: a `<script src="...">` is dropped for now
 * (external fetch/execute is a follow-up). `type` attributes other than
 * text/javascript (e.g. application/json, module) are skipped so their text is
 * not run as code. Matching is case-insensitive on the tag delimiters.
 *
 * `has_inline_handlers`, when non-NULL, is set to whether the page carries an
 * inline event-handler attribute; the caller uses it to decide whether a JS VM
 * is needed at all (see runPageScripts). */
static bool html_has_inline_handlers(const std::string& lower);

/* Read an attribute's value out of a <script> open tag. `lower_tag` is the
 * lower-cased tag (case-insensitive name match), `orig_tag` the same span in
 * original case (URLs are case-sensitive); both start at the same offset, so an
 * offset found in one indexes the other. Matches only a whole attribute name -
 * preceded by start/whitespace and followed by optional space then '=' - so
 * data-src= / srcset= never false-match. Returns "" when absent or empty. */
static std::string script_attr_value(const std::string& lower_tag,
                                     const std::string& orig_tag, const char* name)
{
    size_t nlen = strlen(name);
    size_t pos = 0;
    while(pos < lower_tag.size()) {
        size_t f = lower_tag.find(name, pos);
        if(f == std::string::npos) break;
        bool left_ok = (f == 0) || ::isspace((unsigned char)lower_tag[f - 1]) || lower_tag[f - 1] == '<';
        size_t after = f + nlen;
        while(after < lower_tag.size() && ::isspace((unsigned char)lower_tag[after])) after++;
        if(left_ok && after < lower_tag.size() && lower_tag[after] == '=') {
            size_t v = after + 1;
            while(v < orig_tag.size() && ::isspace((unsigned char)orig_tag[v])) v++;
            std::string val;
            if(v < orig_tag.size() && (orig_tag[v] == '"' || orig_tag[v] == '\'')) {
                char q = orig_tag[v++];
                while(v < orig_tag.size() && orig_tag[v] != q) val += orig_tag[v++];
            } else {
                while(v < orig_tag.size() && !::isspace((unsigned char)orig_tag[v]) && orig_tag[v] != '>') val += orig_tag[v++];
            }
            return val;
        }
        pos = f + 1;
    }
    return std::string();
}

static std::string extract_scripts(const std::string& html, std::vector<std::string>* scripts,
                                   std::vector<std::string>* script_srcs,
                                   bool* has_inline_handlers)
{
    if(has_inline_handlers != nullptr) *has_inline_handlers = false;
    if(html.empty()) {
        return html;
    }

    std::string lower = html;
    for(char& ch : lower) {
        ch = (char)::tolower((unsigned char)ch);
    }
    /* Over the lower-cased copy, before any stripping: <script> bodies are
     * still in it here, and a handler may live anywhere in the markup. */
    if(has_inline_handlers != nullptr) {
        *has_inline_handlers = html_has_inline_handlers(lower);
    }

    std::string out;
    out.reserve(html.size());
    size_t pos = 0;
    int removed = 0;
    while(pos < html.size()) {
        size_t script_open = lower.find("<script", pos);
        if(script_open == std::string::npos) {
            out.append(html, pos, html.size() - pos);
            break;
        }

        /* Find the end of the opening tag so we can inspect its attributes
         * (src=, type=) before deciding whether to run the body. */
        size_t tag_end = lower.find('>', script_open);
        if(tag_end == std::string::npos) {
            /* Malformed: no closing '>' - drop the rest like the old stripper. */
            out.append(html, pos, script_open - pos);
            removed++;
            break;
        }
        std::string open_tag = lower.substr(script_open, tag_end - script_open + 1);
        std::string open_tag_orig = html.substr(script_open, tag_end - script_open + 1);

        out.append(html, pos, script_open - pos);
        size_t script_close = lower.find("</script>", tag_end);
        if(script_close == std::string::npos) {
            removed++;
            break;
        }

        /* Body is the text between '>' and '</script>' (original case). */
        size_t body_start = tag_end + 1;
        std::string body = html.substr(body_start, script_close - body_start);

        std::string src_val = script_attr_value(open_tag, open_tag_orig, "src");
        bool external = !src_val.empty();
        /* A type= that is present and not a JS mime means "do not execute". */
        bool non_js = false;
        size_t tp = open_tag.find("type=");
        if(tp != std::string::npos) {
            non_js = (open_tag.find("javascript", tp) == std::string::npos);
        }
        if(scripts != nullptr && !non_js) {
            if(external) {
                /* External <script src>: reserve an ordered slot with an empty
                 * body (filled when the EWEB_TASK_SCRIPT fetch lands) and record
                 * the src so the caller resolves + queues it. scripts and
                 * script_srcs stay the same length. */
                scripts->push_back(std::string());
                if(script_srcs != nullptr) script_srcs->push_back(src_val);
            } else {
                /* Skip blank/whitespace-only bodies to avoid empty vm_load calls. */
                bool blank = true;
                for(size_t i = 0; i < body.size(); ++i) {
                    if(!::isspace((unsigned char)body[i])) { blank = false; break; }
                }
                if(!blank) {
                    scripts->push_back(body);
                    if(script_srcs != nullptr) script_srcs->push_back(std::string());
                }
            }
        }

        pos = script_close + 9;
        removed++;
    }

    return removed > 0 ? out : html;
}

/* Detect an inline event-handler attribute (`onclick=`, `onload=`, ...) so a
 * page with no <script> block still gets a VM. Only tag-shaped spans are
 * examined - a '<' that starts a name, up to the next '>' - which keeps prose
 * out of the match. An attribute name must start after whitespace and the "on"
 * prefix must be followed by at least one more letter and then '=', so neither
 * `data-onclick=` nor the word "only" can match. Getting this wrong in either
 * direction is cheap: a false positive builds a VM the page never uses, a false
 * negative only loses handlers on a page that has no script block at all. */
static bool html_has_inline_handlers(const std::string& lower)
{
    size_t pos = 0;
    while(pos < lower.size()) {
        size_t open = lower.find('<', pos);
        if(open == std::string::npos) return false;
        if(open + 1 >= lower.size() || !::isalpha((unsigned char)lower[open + 1])) {
            pos = open + 1;   /* '</', '<!', '<?', or a bare '<' inside text */
            continue;
        }
        size_t close = lower.find('>', open + 1);
        if(close == std::string::npos) return false;
        for(size_t i = open + 2; i + 3 < close; ++i) {
            char c = lower[i];
            if(c != ' ' && c != '\t' && c != '\n' && c != '\r') continue;
            if(lower[i + 1] != 'o' || lower[i + 2] != 'n') continue;
            size_t j = i + 3;
            while(j < close && ::isalpha((unsigned char)lower[j])) ++j;
            if(j == i + 3) continue;              /* "on" alone: not a handler */
            while(j < close && (lower[j] == ' ' || lower[j] == '\t')) ++j;
            if(j < close && lower[j] == '=') return true;
        }
        pos = close + 1;
    }
    return false;
}

static uint32_t debug_hash_text(const std::string& text)
{
    uint32_t hash = 2166136261u;
    for(size_t i = 0; i < text.size(); ++i) {
        hash ^= (uint8_t)text[i];
        hash *= 16777619u;
    }
    return hash;
}

/* ------------------------------------------------------------------ */
/* Construction / destruction                                          */
/* ------------------------------------------------------------------ */

/* The init list is kept in declaration order (EWebInternal.h) so that
 * -Wreorder stays quiet: members are constructed in the order they are
 * declared no matter how the init list reads. The std::string / std::vector /
 * std::unordered_map members are left out entirely and default-construct. */
EWebEngine::EWebEngine(const eweb_port_t* port)
    : m_port(port != nullptr ? *port : eweb_port_t())
    , m_container(nullptr)
    , m_doc(nullptr)
    , m_pendingDeleteContainer(nullptr)
    , m_pendingDeleteDoc(nullptr)
    , m_activeContext(&m_browser_context)
    , m_buildPhase(BUILD_IDLE)
    , m_buildContainer(nullptr)
    , m_buildDoc(nullptr)
    , m_buildTargetContext(nullptr)
    , m_task_running(false)
    , m_task_ended(false)
    , m_clientWidth(640)
    , m_clientHeight(480)
    , m_engineThread(0)
    , m_engineStarted(false)
    , m_engineStop(false)
    , m_pendingFrame(nullptr)
    , m_pendingFrameX(0)
    , m_pendingFrameY(0)
    , m_pendingDocW(0)
    , m_pendingDocH(0)
    , m_engineScrollX(0)
    , m_engineScrollY(0)
    , m_needsStyleUpdate(false)
    , m_styleNeedSince(0)
    , m_needsLayout(false)
    , m_pendingCss(0)
    , m_styleStepInFlight(false)
    , m_pressX(0)
    , m_pressY(0)
    , m_pressValid(false)
    , m_buildNeedsStyleUpdate(false)
    , m_buildNeedsLayout(false)
    , m_flushDeferredImages(false)
    , m_defaultCssPrepared(false)
    , m_defaultCssLoading(false)
    , m_deferBuildStep(false)
    , m_layoutDirtyAt(0)
    , m_buildLayoutDirtyAt(0)
    , m_layoutDirtySince(0)
    , m_buildLayoutDirtySince(0)
    , m_pageCache(nullptr)
    , m_cacheScrollX(0)
    , m_cacheScrollY(0)
    , m_cacheValid(false)
    , m_contentDirty(false)
    , m_contentDirtySince(0)
    , m_jsVm(nullptr)
    , m_jsHasInlineHandlers(false)
    , m_jsEnabled(true)
    , m_jsReparseCount(0)
    , m_jsRunBeforePaint(false)
    , m_jsNextScript(0)
    , m_jsScriptWaitSince(0)
    , m_jsPostSwapRun(false)
    , m_jsProgressiveActive(false)
    , m_jsLastFlushAt(0)
    , m_jsFlushCostMs(0)
    , m_jsInScript(false)
    , m_jsEnterAt(0)
    , m_jsEnterGen(0)
    , m_jsAbortCount(0)
    , m_jsPageDisabled(false)
    , m_buildAbort(false)
    , m_buildAbortGen(0)
    , m_jsScrollPending(false)
    , m_jsHoverElement(nullptr)
    , m_focusElement(nullptr)
    , m_openSelect(nullptr)
    , m_rangeDragging(false)
    , m_textDragging(false)
    , m_dragWidget(nullptr)
{
    eweb_listener_init(&m_listener);
    m_container = new EWebContainer(&m_port, this);

    pthread_mutex_init(&m_cssMediaMutex, NULL);
    pthread_mutex_init(&m_taskMutex, NULL);
    pthread_mutex_init(&m_resultMutex, NULL);
    pthread_mutex_init(&m_cmdMutex, NULL);
    pthread_cond_init(&m_cmdCond, NULL);
    pthread_mutex_init(&m_uiMutex, NULL);

    /* Bring the engine thread up last: it exclusively owns everything above
     * and parks on m_cmdCond until the first command arrives, so nothing runs
     * before the engine is fully constructed. */
    engineStart();
}

EWebEngine::~EWebEngine()
{
    /* Termination path. The engine thread owns the documents, the VM and
     * every surface - all non-thread-safe and all built there - so IT tears
     * them down. engineStop() posts ECMD_SHUTDOWN, wakes the loop and joins
     * the thread; the loop runs engineTeardown() in the engine's own context
     * before returning. Only if the thread never came up do we tear down
     * right here. */
    bool engineWasStarted = m_engineStarted;
    engineStop();
    if(!engineWasStarted)
        engineTeardown();

    /* Reap the on-demand download worker. It only touches the mutex-guarded
     * task/result queues and the port's fetch/decode callbacks - never the
     * documents - so stopping it AFTER the engine is safe, and doing it after
     * means no addTask() can respawn it mid-teardown. pthread_detach is a
     * no-op on some targets, so poll m_task_running until the worker clears
     * it on exit. */
    m_task_ended = true;
    while(m_task_running) {
        portSleepMs(10);
    }

    pthread_cond_destroy(&m_cmdCond);
    pthread_mutex_destroy(&m_cmdMutex);
    pthread_mutex_destroy(&m_uiMutex);
    pthread_mutex_destroy(&m_taskMutex);
    pthread_mutex_destroy(&m_resultMutex);
    pthread_mutex_destroy(&m_cssMediaMutex);
}

void EWebEngine::portSleepMs(uint32_t ms)
{
    if(m_port.clock.sleep_ms != nullptr) {
        m_port.clock.sleep_ms(m_port.clock.ud, ms);
        return;
    }
    /* The HAL marks sleep_ms OPTIONAL; fall back to a bounded wait on a
     * never-signalled local condition variable. */
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    pthread_mutex_init(&mtx, NULL);
    pthread_cond_init(&cond, NULL);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t deadline_us = (uint64_t)tv.tv_sec * 1000000ULL +
                           (uint64_t)tv.tv_usec + (uint64_t)ms * 1000ULL;
    struct timespec abstime;
    abstime.tv_sec = (time_t)(deadline_us / 1000000ULL);
    abstime.tv_nsec = (long)((deadline_us % 1000000ULL) * 1000ULL);
    pthread_mutex_lock(&mtx);
    pthread_cond_timedwait(&cond, &mtx, &abstime);
    pthread_mutex_unlock(&mtx);
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mtx);
}

/* ==================================================================
 * Engine thread core + cross-thread channels
 *
 * The engine thread exclusively owns the documents, the containers, the
 * contexts, the mario VM and every surface it draws into, so ALL of their
 * construction, mutation and destruction happens here - including the
 * termination cleanup for stop / navigate / reload / shutdown. The UI thread
 * only signals (postCommand) and joins (engineStop); tick / releaseFrame are
 * the UI-side counterparts further down.
 * ================================================================== */

static void* _ew_engine_thread(void* p)
{
    /* Trampoline: the loop body is a member (it reaches private state), so hop
     * through the engine. Returns once engineLoop() breaks on ECMD_SHUTDOWN,
     * after engineTeardown() has freed everything the engine owns - which is
     * why the destructor can join and then only destroy the mutexes. */
    EWebEngine* engine = (EWebEngine*)p;
    if(engine != nullptr)
        engine->engineLoop();
    return nullptr;
}

void EWebEngine::engineStart()
{
    /* Spawn the engine thread. Called at the END of the constructor so every
     * member it touches is already initialised; the thread parks on m_cmdCond
     * until the first command arrives (setDefaultCSS is a direct setter, the
     * first load posts ECMD_NAVIGATE), so nothing runs before the engine is
     * whole. If the spawn fails the engine still works as a blank viewport:
     * the destructor sees m_engineStarted == false and tears down inline. */
    m_engineStop = false;
    if(pthread_create(&m_engineThread, NULL, _ew_engine_thread, this) != 0) {
        EWEB_LOG("[ewebview] engine thread create FAILED\n");
        m_engineStarted = false;
        return;
    }
    m_engineStarted = true;
}

void EWebEngine::engineStop()
{
    /* Signal the loop to stop, wake it with a SHUTDOWN command and join. The
     * loop runs engineTeardown() in the engine's own context before
     * returning, so every non-thread-safe object (documents, VM, surfaces) is
     * destroyed where it was built. Safe to call when the thread never came
     * up - the destructor then tears down inline instead. */
    if(!m_engineStarted)
        return;
    m_engineStop = true;
    EWebCmd cmd;
    cmd.kind = ECMD_SHUTDOWN;
    postCommand(cmd);
    pthread_join(m_engineThread, NULL);
    m_engineStarted = false;
}

void EWebEngine::postCommand(const EWebCmd& cmd)
{
    /* Any thread (in practice the UI thread: load/stop/reload, viewport
     * resize, scroll, input). Push onto m_cmdQueue and wake the engine.
     *
     * A termination command (STOP / NAVIGATE / RELOAD / SHUTDOWN) ALSO raises
     * the volatile m_buildAbort right here, BEFORE the queue lock: the single
     * engine thread cannot interrupt itself mid-createFromString / render /
     * vm_run, so this cross-thread signal is what lets a long parse or a
     * hostile script unwind the instant the user hits stop/back/reload
     * (jsOnVmStep and the EWebContainer hot callbacks poll
     * buildAbortRequested()). The engine clears the flag in
     * cleanupBuildResources() once the abort is consumed. */
    bool terminate = (cmd.kind == ECMD_STOP || cmd.kind == ECMD_NAVIGATE ||
                      cmd.kind == ECMD_RELOAD || cmd.kind == ECMD_SHUTDOWN);
    if(terminate) {
        m_buildAbort = true;
        /* Bump the generation so a VM run in flight RIGHT NOW (snapshotted in
         * jsVmEnter) can tell this fresh abort apart from a stale one. */
        m_buildAbortGen++;
    }

    pthread_mutex_lock(&m_cmdMutex);
    m_cmdQueue.push_back(cmd);
    pthread_cond_signal(&m_cmdCond);
    pthread_mutex_unlock(&m_cmdMutex);
}

void EWebEngine::postUiEvent(const EWebUiEvent& ev)
{
    /* Engine thread (and the download worker, for task status). Append to
     * m_uiQueue under the narrow UI mutex; the UI thread drains it in tick().
     * Only a POD/string copy crosses - never a call into litehtml, the VM or
     * the port - so the engine is never blocked behind embedder work and vice
     * versa. */
    pthread_mutex_lock(&m_uiMutex);
    m_uiQueue.push_back(ev);
    pthread_mutex_unlock(&m_uiMutex);
}

void EWebEngine::postScrollClamp()
{
    /* ENGINE-THREAD ONLY. Publish the engine's authoritative scroll offset and
     * the visible document's geometry as EUET_SCROLL_CLAMP so the UI thread
     * snaps its live offset to it and refreshes the scrollbar. Used on a page
     * swap (offset reset to 0), when a script scrolls (window.scrollTo /
     * scrollBy), and when a post-swap script run ends. */
    int docW = m_doc ? m_doc->width() : 0;
    int docH = m_doc ? m_doc->height() : 0;
    clampScrollLocked(docW, docH);
    EWebUiEvent ev;
    ev.kind = EUET_SCROLL_CLAMP;
    ev.x = m_engineScrollX;
    ev.y = m_engineScrollY;
    ev.w = docW;
    ev.h = docH;
    postUiEvent(ev);
}

void EWebEngine::engineLoop()
{
    /* The engine thread body. Each iteration:
     *   1. drain UI->engine commands (the termination cleanup runs here);
     *   2. flush deferred doc/container deletions from a previous swap;
     *   3. drain the download result queue into the build/visible doc;
     *   4. apply pending style/layout;
     *   5. drive the build state machine one step;
     *   6. service a script-requested navigation/scroll;
     *   7. poll JS timers (setInterval/setTimeout);
     *   8. flush images deferred during a build;
     *   9. render a frame if the page or the scroll offset changed;
     *  10. park on m_cmdCond when idle.
     * It NEVER touches the embedder's window. */
    while(!m_engineStop) {
        /* 1. commands. Drain the whole queue so a burst (resize + input +
         * stop) is handled in order before any rendering.
         * engineHandleCommand returns false only for ECMD_SHUTDOWN. */
        bool shutdown = false;
        for(;;) {
            pthread_mutex_lock(&m_cmdMutex);
            if(m_cmdQueue.empty()) {
                pthread_mutex_unlock(&m_cmdMutex);
                break;
            }
            EWebCmd cmd = m_cmdQueue.front();
            m_cmdQueue.pop_front();
            pthread_mutex_unlock(&m_cmdMutex);
            if(!engineHandleCommand(cmd)) {
                shutdown = true;
                break;
            }
        }
        if(shutdown)
            break;

        /* 2. deferred deletions parked by the last BUILD_SWAP_DOC (freeing
         * them inside the swap would re-enter the container while it is still
         * live). */
        if(m_pendingDeleteDoc) { delete m_pendingDeleteDoc; m_pendingDeleteDoc = nullptr; }
        if(m_pendingDeleteContainer) { delete m_pendingDeleteContainer; m_pendingDeleteContainer = nullptr; }

        /* 3. download results. Drain everything that has landed; stop early
         * if a termination was raised mid-drain so cleanup is not delayed. */
        bool more = processResults();
        while(more && !m_buildAbort) {
            more = processResults();
        }

        /* 4. style/layout for both the build and the visible doc. */
        if(applyPendingLayoutUpdates())
            markContentDirty();

        /* 5. build state machine (one phase step per iteration).
         * m_deferBuildStep skips a step right after a result drove the build,
         * so the results settle first. */
        if(m_buildPhase != BUILD_IDLE) {
            if(m_deferBuildStep) {
                m_deferBuildStep = false;
            } else {
                advanceBuildStep();
            }
        }

        /* 6. a navigation/scroll a script or an anchor click asked for. Runs
         * after the build step so a location.href set during a post-swap
         * script is honoured once that step unwinds. */
        jsRunPendingNavigation();

        /* 7. setInterval()/setTimeout(). jsPollTimers returns how many fired;
         * a callback may have drawn (canvas) without touching layout, so any
         * firing marks the content dirty to force a full re-render. A fresh
         * termination abort unwinds a runaway callback via the jsOnVmStep
         * generation check, so polling never has to be skipped here - and
         * MUST not be skipped on a stale flag, or the visible page's timers
         * would freeze after a STOP on an idle page. */
        if(jsPollTimers() > 0)
            markContentDirty();

        /* 8. images whose load was deferred during the build (set by
         * BUILD_SWAP_DOC): mount them now that the page is on screen. */
        if(m_flushDeferredImages && m_doc && m_container) {
            m_container->flushPendingImages();
            m_flushDeferredImages = false;
            markContentDirty();
        }

        /* 9. render the viewport if the page changed or the scroll offset
         * moved away from what the cache holds. engineRenderFrame is a no-op
         * while a previous frame still awaits adoption (back-pressure). */
        if(m_doc && (!m_cacheValid || m_contentDirty ||
                     m_engineScrollX != m_cacheScrollX ||
                     m_engineScrollY != m_cacheScrollY)) {
            engineRenderFrame(true);
        }

        /* 10. park when there is nothing to do, so an idle page costs no CPU.
         * The wait is ALWAYS bounded (timed) as a lost-wakeup safety net; the
         * timeout is chosen by state, and postCommand()/pushResult()/
         * releaseFrame() signal m_cmdCond so a real event wakes the loop
         * immediately. */
        bool blocked_on_adopt = false;
        pthread_mutex_lock(&m_uiMutex);
        blocked_on_adopt = (m_pendingFrame != nullptr);
        pthread_mutex_unlock(&m_uiMutex);

        bool busy = (m_buildPhase != BUILD_IDLE) ||
                    m_contentDirty || m_needsLayout || m_needsStyleUpdate ||
                    m_buildNeedsLayout || m_buildNeedsStyleUpdate ||
                    m_styleStepInFlight || m_deferBuildStep || m_flushDeferredImages ||
                    !m_jsPendingNav.empty() || m_jsScrollPending ||
                    (m_doc && (m_engineScrollX != m_cacheScrollX ||
                               m_engineScrollY != m_cacheScrollY || !m_cacheValid));
        bool vm_active = (m_jsVm != nullptr && m_jsEnabled && !m_jsPageDisabled);

        uint32_t timeout_ms;
        if(blocked_on_adopt)     timeout_ms = 16;   /* waiting for the UI to adopt */
        else if(busy)            timeout_ms = 4;    /* pace a build/wait, no pure spin */
        else if(vm_active)       timeout_ms = 16;   /* timer granularity */
        else                     timeout_ms = 200;  /* fully idle */

        /* Re-check the result queue before a long park (a fetch may have
         * landed since step 3). Lock order is result-then-cmd and never
         * nested, so it cannot deadlock with pushResult (result-only) or
         * postCommand (cmd-only). */
        pthread_mutex_lock(&m_resultMutex);
        bool have_results = !m_resultQueue.empty();
        pthread_mutex_unlock(&m_resultMutex);
        if(!have_results) {
            pthread_mutex_lock(&m_cmdMutex);
            if(m_cmdQueue.empty() && !m_engineStop) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                uint64_t deadline_us = (uint64_t)tv.tv_sec * 1000000ULL +
                                       (uint64_t)tv.tv_usec + (uint64_t)timeout_ms * 1000ULL;
                struct timespec abstime;
                abstime.tv_sec = (time_t)(deadline_us / 1000000ULL);
                abstime.tv_nsec = (long)((deadline_us % 1000000ULL) * 1000ULL);
                pthread_cond_timedwait(&m_cmdCond, &m_cmdMutex, &abstime);
            }
            pthread_mutex_unlock(&m_cmdMutex);
        }
    }

    /* Loop exit (ECMD_SHUTDOWN or m_engineStop): free everything the engine
     * owns in its own context before the thread returns and the destructor
     * joins. */
    engineTeardown();
}

/* Convert a viewport-relative client point (cx,cy) into a form control's
 * BORDER-box local coordinates, using the control's document placement and the
 * engine's scroll offset. Shared by every mouse-driven widget interaction. */
static void widgetLocalCoords(eweb_el_input* w, int cx, int cy,
                              int scrollX, int scrollY, int& lx, int& ly)
{
    litehtml::position pl = ((litehtml::element*)w)->get_placement();
    lx = cx + scrollX - pl.x;
    ly = cy + scrollY - pl.y;
}

/* Collect every form control (eweb_el_input) in the subtree under `e`, in
 * document order. Used for radio-group clearing and form field collection. */
static void collectWidgets(litehtml::element* e, std::vector<eweb_el_input*>& out)
{
    if(e == nullptr) return;
    void* w = e->eweb_form_widget();
    if(w != nullptr) out.push_back((eweb_el_input*)w);
    size_t n = e->get_children_count();
    for(size_t i = 0; i < n; ++i)
        collectWidgets(e->get_child((int)i), out);
}

/* application/x-www-form-urlencoded: space -> '+', unreserved verbatim,
 * everything else %XX. */
static std::string eweb_urlencode(const char* s)
{
    std::string out;
    if(s == nullptr) return out;
    for(const unsigned char* p = (const unsigned char*)s; *p != 0; ++p) {
        unsigned char c = *p;
        if(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if(c == ' ') out += '+';
        else { char b[4]; snprintf(b, sizeof(b), "%%%02X", c); out += b; }
    }
    return out;
}

bool EWebEngine::engineHandleCommand(const EWebCmd& cmd)
{
    /* ENGINE-THREAD ONLY. Consume one UI->engine command. Returns false only
     * for ECMD_SHUTDOWN, which breaks the loop and triggers engineTeardown().
     * The four termination triggers (STOP/NAVIGATE/RELOAD/SHUTDOWN) perform
     * the cleanup here, on the thread that owns the documents/VM/surfaces. */
    switch(cmd.kind) {
    case ECMD_NAVIGATE:
        /* Address bar / initial load / script-driven navigation. */
        engineNavigate(cmd.url);
        return true;

    case ECMD_RELOAD:
        /* Re-navigate to the page currently on screen. */
        engineNavigate(m_currentHtmlUrl);
        return true;

    case ECMD_STOP:
        /* Interrupt the in-flight build/fetches and discard the half-built
         * page, keeping whatever is already on screen. */
        engineAbortBuild();
        engineDropPendingWork();
        return true;

    case ECMD_RESIZE: {
        /* The viewport changed size. The engine owns the client size, the
         * container, the document and window.onresize (which runs JS). */
        if(cmd.x > 0) m_clientWidth = cmd.x;
        if(cmd.y > 0) m_clientHeight = cmd.y;
        if(m_container) m_container->set_client_size(m_clientWidth, m_clientHeight);
        if(m_buildContainer) m_buildContainer->set_client_size(m_clientWidth, m_clientHeight);
        /* The frame pool is viewport-sized: drop it so engineRenderFrame
         * reallocates at the new size. The embedder drops its stale front
         * buffer when it adopts the first correctly-sized frame. */
        engineFreeFramePool();
        m_cacheValid = false;
        if(m_doc) {
            m_doc->render(m_clientWidth);
            clampScrollLocked(m_doc->width(), m_doc->height());
            markContentDirty();
        }
        if(m_buildDoc) {
            markLayoutDirty(true);
        }
        postScrollClamp();
        jsFireResizeEvent();
        return true;
    }

    case ECMD_SCROLL:
        /* The UI moved its live offset (wheel/drag); adopt it as the engine's
         * authoritative offset, re-render the exposed content and fire the
         * page's scroll handlers here (where the document lives). */
        m_engineScrollX = cmd.x;
        m_engineScrollY = cmd.y;
        clampScrollLocked(m_doc ? m_doc->width() : 0, m_doc ? m_doc->height() : 0);
        markContentDirty();
        jsFireScrollEvent();
        return true;

    case ECMD_INPUT: {
        /* Dispatch the DOM mouse events and drive the form-control default
         * actions. cx/cy are the viewport-relative client coords the embedder
         * pre-computed (the engine cannot walk the embedder's window geometry).
         * A local copy is taken because the dispatch may outlive `cmd`. */
        eweb_event_t ev = cmd.ev;
        int cx = cmd.x, cy = cmd.y;

        /* An open <select> popup floats above the page and is NOT part of the
         * litehtml tree, so it captures the pointer before the page hit-test. */
        if(m_openSelect != nullptr) {
            eweb_el_input* sel = (eweb_el_input*)m_openSelect;
            litehtml::position r; int rowH = 0, visible = 0;
            bool have = selectPopupRect(r, rowH, visible);
            bool inside = have && rowH > 0 &&
                          cx >= r.x && cx < r.x + r.width &&
                          cy >= r.y && cy < r.y + r.height;
            if(ev.mouse_state == EWEB_MOUSE_MOVE) {
                if(inside) {
                    int idx = (cy - r.y - 1) / rowH;
                    if(idx < 0) idx = 0;
                    if(idx >= sel->dropdownRowCount()) idx = sel->dropdownRowCount() - 1;
                    if(idx != sel->activeOption()) { sel->setActiveOption(idx); markContentDirty(); }
                }
                return true;   /* popup open: swallow hover, never hit the page */
            }
            if(ev.mouse_state == EWEB_MOUSE_DOWN) {
                if(inside && have) {
                    int idx = (cy - r.y - 1) / rowH;
                    if(idx >= 0 && idx < sel->dropdownRowCount()) {
                        sel->setActiveOption(idx);
                        markContentDirty();
                    }
                } else {
                    sel->toggleDropdown();   /* a click outside closes it */
                    m_openSelect = nullptr;
                    markContentDirty();
                }
                return true;
            }
            if(ev.mouse_state == EWEB_MOUSE_UP) {
                if(inside && have) {
                    int idx = (cy - r.y - 1) / rowH;
                    if(idx >= 0 && idx < sel->dropdownRowCount()) {
                        sel->setActiveOption(idx);
                        sel->chooseActiveOption();
                        m_openSelect = nullptr;
                        fireWidgetEvent((litehtml::element*)sel, "input");
                        fireWidgetEvent((litehtml::element*)sel, "change");
                        markContentDirty();
                    }
                }
                return true;
            }
            /* wheel / other states fall through to the page dispatch below */
        }

        bool allowed = jsDispatchMouseEvent(ev.mouse_state, ev.button, cx, cy);

        if(ev.mouse_state == EWEB_MOUSE_DOWN && ev.button == EWEB_BUTTON_LEFT) {
            /* Click-to-focus: a left press moves keyboard focus to the nearest
             * focusable element under the pointer (a form widget or <a href>),
             * or clears focus when the press lands on blank space. Runs AFTER
             * the DOM mouse events so a mousedown handler sees the old focus,
             * matching the browser order (focus changes between mousedown and
             * click). */
            litehtml::element* target = hitElementAt(cx, cy);
            setFocus(target != nullptr ? focusableAt(target) : nullptr);
            /* Caret placement + drag start for text fields and range sliders. */
            void* wv = (target != nullptr) ? widgetAt(target) : nullptr;
            if(wv != nullptr) {
                eweb_el_input* w = (eweb_el_input*)wv;
                int lx, ly;
                widgetLocalCoords(w, cx, cy, m_engineScrollX, m_engineScrollY, lx, ly);
                if(w->isTextEditing()) {
                    w->placeCaretAt(lx, ly);
                    m_textDragging = true;
                    m_dragWidget = wv;
                    markContentDirty();
                } else if(w->inputType() == EWEB_INPUT_RANGE) {
                    w->setRangeFromX(lx);
                    m_rangeDragging = true;
                    m_dragWidget = wv;
                    fireWidgetEvent((litehtml::element*)w, "input");
                    markContentDirty();
                }
            }
            /* Click-vs-drag bookkeeping for the anchor follow (below). */
            m_pressX = cx;
            m_pressY = cy;
            m_pressValid = true;
        } else if(ev.mouse_state == EWEB_MOUSE_DOUBLE_CLICK &&
                  ev.button == EWEB_BUTTON_LEFT) {
            litehtml::element* target = hitElementAt(cx, cy);
            void* wv = (target != nullptr) ? widgetAt(target) : nullptr;
            if(wv != nullptr && ((eweb_el_input*)wv)->isTextEditing()) {
                int lx, ly;
                widgetLocalCoords((eweb_el_input*)wv, cx, cy,
                                  m_engineScrollX, m_engineScrollY, lx, ly);
                ((eweb_el_input*)wv)->selectWordAtLocal(lx);
                markContentDirty();
            }
        } else if(ev.mouse_state == EWEB_MOUSE_MOVE) {
            if(m_dragWidget != nullptr && (m_rangeDragging || m_textDragging)) {
                eweb_el_input* w = (eweb_el_input*)m_dragWidget;
                int lx, ly;
                widgetLocalCoords(w, cx, cy, m_engineScrollX, m_engineScrollY, lx, ly);
                if(m_rangeDragging) {
                    w->setRangeFromX(lx);
                    fireWidgetEvent((litehtml::element*)w, "input");
                } else {
                    w->dragSelectTo(lx);
                }
                markContentDirty();
            }
        } else if(ev.mouse_state == EWEB_MOUSE_UP && ev.button == EWEB_BUTTON_LEFT) {
            /* End a drag: a completed range/text edit commits with "change". */
            if(m_dragWidget != nullptr && (m_rangeDragging || m_textDragging))
                fireWidgetEvent((litehtml::element*)(eweb_el_input*)m_dragWidget, "change");
            m_rangeDragging = false;
            m_textDragging = false;
            m_dragWidget = nullptr;
            /* The anchor follow lives here, NOT inside jsDispatchMouseEvent:
             * link clicking must keep working on pages with no VM (or JS
             * disabled), where the dispatch returns early. preventDefault
             * suppresses the follow. */
            if(allowed)
                handleAnchorClick(cx, cy);
            else
                m_pressValid = false;
        } else if(ev.mouse_state == EWEB_MOUSE_CLICK && ev.button == EWEB_BUTTON_LEFT) {
            /* Activation behaviour (checkbox/radio/select/button) runs on the
             * click, gated on the DOM click handler not cancelling it. Text and
             * range were already handled on the press/drag. */
            if(allowed) {
                litehtml::element* target = hitElementAt(cx, cy);
                void* wv = (target != nullptr) ? widgetAt(target) : nullptr;
                if(wv != nullptr) {
                    int lx, ly;
                    widgetLocalCoords((eweb_el_input*)wv, cx, cy,
                                      m_engineScrollX, m_engineScrollY, lx, ly);
                    activateWidget(wv, lx, ly);
                }
            }
        }
        return true;
    }

    case ECMD_KEY:
        /* Dispatch the DOM key events and run the default action (editing,
         * activation, focus traversal) on the engine thread, where the
         * document and VM live. A local copy is taken because the dispatch
         * may outlive `cmd`. */
        handleKeyEvent(cmd.kev);
        return true;

    case ECMD_SET_JS:
        /* Toggle JS execution. Disabling frees the VM; the next page load
         * honours the new setting (extract_scripts drops <script> bodies when
         * disabled). */
        m_jsEnabled = cmd.b;
        if(!m_jsEnabled)
            resetJsVm();
        return true;

    case ECMD_SHUTDOWN:
        return false;   /* break the loop; engineTeardown runs on the way out */

    default:
        return true;
    }
}

void EWebEngine::engineNavigate(const std::string& url)
{
    /* ENGINE-THREAD ONLY. Backs ECMD_NAVIGATE / ECMD_RELOAD and script-driven
     * navigation (jsRunPendingNavigation). Terminate the page being left:
     * drop every queued/landed download, then reset the build machine (which
     * frees the old page's VM, canvases, detached nodes and any half-built
     * doc), reset the scroll offset, point the SameSite initiator at the page
     * being left and queue the HTML fetch.
     *
     * The visible page (m_doc) is deliberately NOT freed here - it stays on
     * screen under the build overlay until BUILD_SWAP_DOC replaces it and
     * defers its deletion to a later loop iteration. */
    engineDropPendingWork();
    cleanupBuildResources();

    m_engineScrollX = 0;
    m_engineScrollY = 0;
    m_cacheValid = false;

    /* Alternate the build context so the new page's master stylesheet set
     * starts clean without disturbing the one the visible page is still
     * using. */
    m_buildTargetContext = (m_activeContext == &m_browser_context) ? &m_buildContext : &m_browser_context;
    m_buildTargetContext->master_css().clear();
    pthread_mutex_lock(&m_cssMediaMutex);
    m_cssMedia.clear();
    pthread_mutex_unlock(&m_cssMediaMutex);

    /* The page being left is the initiator of this navigation, which is what
     * SameSite is defined against; loadHtmlContent moves it on to the new
     * document once the HTML arrives. */
    setTaskPageUrl(m_currentHtmlUrl);

    EWebTask task;
    task.url = url;
    task.type = EWEB_TASK_HTML;
    task.loading = false;
    addTask(task);
    EWEB_LOG("[ewebview] engine navigate -> %s\n", url.c_str());
}

void EWebEngine::engineAbortBuild()
{
    /* ENGINE-THREAD ONLY. Interrupt the in-flight parse/style/layout/VM run
     * and discard the half-built page. requestBuildAbort() raises
     * m_buildAbort + the build container's short-circuit flag (so a call the
     * engine is nested in unwinds fast), and cleanupBuildResources() frees
     * the build doc/container/VM/canvases and returns the machine to
     * BUILD_IDLE.
     *
     * The visible page (m_doc) is left untouched, so a STOP keeps whatever is
     * already on screen. When nothing was in flight the abort flag stays
     * raised (cleanupBuildResources is skipped), which kills a late in-flight
     * fetch result that would otherwise restart the abandoned build. */
    requestBuildAbort();
    if(m_buildPhase != BUILD_IDLE || m_buildDoc != nullptr || m_buildContainer != nullptr) {
        cleanupBuildResources();
    }
}

void EWebEngine::engineDropPendingWork()
{
    /* ENGINE-THREAD ONLY. Discard the downloads queued or already fetched for
     * the page being terminated:
     *   - empty the not-yet-started task queue, releasing a dropped
     *     stylesheet's m_pendingCss slot and seen-CSS entry so the visible
     *     doc's style walk does not stall behind sheets that will never
     *     arrive and a reload does not skip them as duplicates;
     *   - drop the result queue, freeing any decoded image surface the worker
     *     produced but the engine never mounted, so a stop does not leak the
     *     hand-off.
     * The ONE in-flight fetch (loading==true) cannot be cancelled
     * mid-transfer; its result still lands and is processed, so a stopped
     * page keeps whatever already arrived. Both queues are guarded by the
     * mutexes shared with the worker. */
    pthread_mutex_lock(&m_taskMutex);
    for(size_t i = 0; i < m_taskQueue.size(); ) {
        if(!m_taskQueue[i].loading) {
            if(m_taskQueue[i].type == EWEB_TASK_CSS) {
                forgetCSS(m_taskQueue[i].url);
                if(m_pendingCss > 0)
                    m_pendingCss--;
            }
            m_taskQueue.erase(m_taskQueue.begin() + i);
        } else {
            ++i;
        }
    }
    pthread_mutex_unlock(&m_taskMutex);

    pthread_mutex_lock(&m_resultMutex);
    for(size_t i = 0; i < m_resultQueue.size(); ++i) {
        if(m_resultQueue[i].image != nullptr) {
            m_port.gfx.surface_free(m_port.gfx.ud, m_resultQueue[i].image);
            m_resultQueue[i].image = nullptr;
        }
    }
    m_resultQueue.clear();
    pthread_mutex_unlock(&m_resultMutex);
}

void EWebEngine::engineTeardown()
{
    /* ENGINE-THREAD ONLY, runs once as the loop exits (ECMD_SHUTDOWN) or, if
     * the thread never came up, from the destructor. Free everything the
     * engine owns in an order that respects the back-references: downloads
     * first, then the VM/element handles (they point into the documents, so
     * they must die first), then the build and visible documents/containers,
     * then the deferred-delete pair, then the frame pool. An adopted frame
     * the embedder still holds is the embedder's problem - the public API
     * requires releasing adopted frames before ewebview_destroy(). */
    engineDropPendingWork();
    cleanupBuildResources();   /* frees VM, canvases, detached nodes, build doc */

    if(m_doc) { delete m_doc; m_doc = nullptr; }
    if(m_container) { delete m_container; m_container = nullptr; }
    if(m_pendingDeleteDoc) { delete m_pendingDeleteDoc; m_pendingDeleteDoc = nullptr; }
    if(m_pendingDeleteContainer) { delete m_pendingDeleteContainer; m_pendingDeleteContainer = nullptr; }

    engineFreeFramePool();
}

void EWebEngine::engineEnsureFramePool()
{
    /* ENGINE-THREAD ONLY. Keep at least one viewport-sized surface in
     * m_freeFrames for engineRenderFrame to draw into, and drop any spare
     * whose size no longer matches the client (a stale buffer the embedder
     * returned after a resize). Guarded by m_uiMutex because the UI thread
     * returns buffers to m_freeFrames there (releaseFrame). */
    int w = m_clientWidth > 0 ? m_clientWidth : 1;
    int h = m_clientHeight > 0 ? m_clientHeight : 1;
    pthread_mutex_lock(&m_uiMutex);
    for(size_t i = 0; i < m_freeFrames.size(); ) {
        eweb_surface_t* b = m_freeFrames[i];
        if(b == nullptr || surfaceW(b) != w || surfaceH(b) != h) {
            if(b) m_port.gfx.surface_free(m_port.gfx.ud, b);
            m_freeFrames.erase(m_freeFrames.begin() + i);
        } else {
            ++i;
        }
    }
    if(m_freeFrames.empty()) {
        eweb_surface_t* b = m_port.gfx.surface_new(m_port.gfx.ud, w, h);
        if(b != nullptr)
            m_freeFrames.push_back(b);
    }
    pthread_mutex_unlock(&m_uiMutex);
}

void EWebEngine::engineFreeFramePool()
{
    /* ENGINE-THREAD ONLY. Release the engine side of the pool: the free
     * spares, any frame awaiting UI adoption (m_pendingFrame) and the buffer
     * currently being drawn (m_pageCache, normally null outside a render).
     * Frames the embedder has already adopted are its own. The pending frame
     * is freed under m_uiMutex so a UI tick that reads it cannot race the
     * free. */
    pthread_mutex_lock(&m_uiMutex);
    for(size_t i = 0; i < m_freeFrames.size(); ++i) {
        if(m_freeFrames[i] != nullptr)
            m_port.gfx.surface_free(m_port.gfx.ud, m_freeFrames[i]);
    }
    m_freeFrames.clear();
    if(m_pendingFrame != nullptr) {
        m_port.gfx.surface_free(m_port.gfx.ud, m_pendingFrame);
        m_pendingFrame = nullptr;
    }
    pthread_mutex_unlock(&m_uiMutex);
    if(m_pageCache != nullptr) {
        m_port.gfx.surface_free(m_port.gfx.ud, m_pageCache);
        m_pageCache = nullptr;
    }
    m_cacheValid = false;
}

void EWebEngine::engineRenderFrame(bool full)
{
    /* ENGINE-THREAD ONLY. Rasterize the visible doc into a pool surface at
     * the current engine scroll offset and post it to the UI as EUET_FRAME.
     * Skips while a previous frame still awaits adoption (m_pendingFrame !=
     * null) - natural back-pressure so a fast script/scroll cannot outrun the
     * display or grow the pool. `full` is currently always true: pool buffers
     * are recycled and hold a stale page, so every draw covers the whole
     * viewport; the embedder's blit-shift covers the interim during a
     * scroll. */
    (void)full;
    if(m_doc == nullptr)
        return;

    pthread_mutex_lock(&m_uiMutex);
    bool pending = (m_pendingFrame != nullptr);
    pthread_mutex_unlock(&m_uiMutex);
    if(pending)
        return;   /* UI has not adopted the last frame yet */

    engineEnsureFramePool();

    pthread_mutex_lock(&m_uiMutex);
    eweb_surface_t* buf = nullptr;
    if(!m_freeFrames.empty()) {
        buf = m_freeFrames.back();
        m_freeFrames.pop_back();
    }
    pthread_mutex_unlock(&m_uiMutex);
    if(buf == nullptr)
        return;   /* allocation failed; try again next iteration */

    m_pageCache = buf;
    int docW = m_doc->width();
    int docH = m_doc->height();
    clampScrollLocked(docW, docH);
    drawPageToCacheLocked(0, surfaceH(buf));
    m_pageCache = nullptr;

    m_cacheScrollX = m_engineScrollX;
    m_cacheScrollY = m_engineScrollY;
    m_cacheValid = true;
    m_contentDirty = false;
    m_contentDirtySince = 0;

    pthread_mutex_lock(&m_uiMutex);
    m_pendingFrame = buf;
    m_pendingFrameX = m_engineScrollX;
    m_pendingFrameY = m_engineScrollY;
    m_pendingDocW = docW;
    m_pendingDocH = docH;
    pthread_mutex_unlock(&m_uiMutex);

    EWebUiEvent ev;
    ev.kind = EUET_FRAME;
    ev.x = m_engineScrollX;
    ev.y = m_engineScrollY;
    ev.w = docW;
    ev.h = docH;
    postUiEvent(ev);
}

/* ==================================================================
 * UI thread: the other end of the engine->UI channel + frame recycling.
 * ================================================================== */

void EWebEngine::setListener(const eweb_listener_t* listener)
{
    /* UI-THREAD (ewebview_set_listener). Copy under the narrow UI mutex;
     * tick() snapshots it the same way, so a listener swapped mid-tick takes
     * effect on the next tick and never mid-callback. */
    pthread_mutex_lock(&m_uiMutex);
    if(listener != nullptr)
        m_listener = *listener;
    else
        eweb_listener_init(&m_listener);
    pthread_mutex_unlock(&m_uiMutex);
}

void EWebEngine::tick()
{
    /* UI-THREAD ONLY (ewebview_tick). Consume the engine->UI queue. Move the
     * whole queue out under the narrow mutex (so the engine is never blocked
     * appending while we process) and snapshot the listener, then fire each
     * callback OUTSIDE the lock: the listener runs embedding-app code that
     * may call back into the engine (e.g. load -> postCommand), which must
     * not deadlock on m_uiMutex. This is the ONLY place the embedder is
     * called back, so every listener hook fires on the UI thread.
     * (The minimal deque has no swap(): copy out, then clear - the queue
     * holds a handful of small events per tick, so the copy is cheap.) */
    std::deque<EWebUiEvent> events;
    eweb_listener_t listener;
    pthread_mutex_lock(&m_uiMutex);
    events = m_uiQueue;
    m_uiQueue.clear();
    listener = m_listener;
    pthread_mutex_unlock(&m_uiMutex);

    for(size_t i = 0; i < events.size(); ++i) {
        const EWebUiEvent& ev = events[i];
        switch(ev.kind) {
        case EUET_FRAME: {
            /* Adopt the freshly rendered surface (m_pendingFrame); its offset
             * and geometry are mirrored in the event. Ownership passes to the
             * listener; with no on_frame hook the surface goes straight back
             * to the pool so the engine is not starved. */
            pthread_mutex_lock(&m_uiMutex);
            eweb_surface_t* frame = m_pendingFrame;
            m_pendingFrame = nullptr;
            pthread_mutex_unlock(&m_uiMutex);
            if(frame == nullptr)
                break;   /* stale event (a resize freed it engine-side) */
            if(listener.on_frame != nullptr)
                listener.on_frame(listener.ud, frame, ev.x, ev.y, ev.w, ev.h);
            else
                releaseFrame(frame);
            /* Wake the engine so it can render the next frame immediately
             * rather than waiting out its blocked-on-adopt park. A signal
             * with no mutex held is safe here; a missed wake is covered by
             * the engine's bounded timedwait. */
            pthread_cond_signal(&m_cmdCond);
            break;
        }
        case EUET_SCROLL_CLAMP:
            /* Engine-authoritative scroll (page swap -> 0, window.scrollTo,
             * or a script run end). */
            if(listener.on_scroll != nullptr)
                listener.on_scroll(listener.ud, ev.x, ev.y, ev.w, ev.h);
            break;
        case EUET_URL:
            /* The visible page's URL changed. Mirror it for
             * ewebview_get_url() and fire the hook. */
            m_uiCurrentUrl = ev.text;
            if(listener.on_url != nullptr)
                listener.on_url(listener.ud, ev.text.c_str());
            break;
        case EUET_BUILD_STATUS:
            /* Build status text + progress; ev.w is the build-overlay flag. */
            if(listener.on_status != nullptr)
                listener.on_status(listener.ud, ev.text.c_str(), ev.progress);
            if(listener.on_build_status != nullptr)
                listener.on_build_status(listener.ud, ev.text.c_str(), ev.progress, ev.w != 0);
            break;
        case EUET_TITLE:
            if(listener.on_title != nullptr)
                listener.on_title(listener.ud, ev.text.c_str());
            break;
        case EUET_DIALOG:
            /* alert()/confirm()/prompt() text, surfaced non-blocking (the
             * engine never waits on a modal; the bridge keeps its
             * default/cancel reply). */
            if(listener.on_dialog != nullptr)
                listener.on_dialog(listener.ud, ev.text.c_str());
            break;
        case EUET_TASK_START:
            if(listener.on_task_start != nullptr)
                listener.on_task_start(listener.ud, ev.task.url.c_str(), ev.task.type);
            break;
        case EUET_TASK_END:
            if(listener.on_task_end != nullptr)
                listener.on_task_end(listener.ud, ev.task.url.c_str(), ev.task.type);
            break;
        case EUET_TASK_FAILED:
            if(listener.on_task_failed != nullptr)
                listener.on_task_failed(listener.ud, ev.task.url.c_str(), ev.task.type);
            break;
        case EUET_TASKS_END:
            if(listener.on_tasks_end != nullptr)
                listener.on_tasks_end(listener.ud);
            break;
        default:
            break;
        }
    }
}

void EWebEngine::releaseFrame(eweb_surface_t* frame)
{
    /* UI-THREAD ONLY (ewebview_release_frame). Return an adopted frame to the
     * pool. No size check here: the gfx table is engine-thread-only per the
     * HAL contract, and engineEnsureFramePool() revalidates every pooled
     * buffer against the current viewport size (surface_dims) before reuse,
     * so a stale-sized return is dropped there instead. */
    if(frame == nullptr)
        return;
    pthread_mutex_lock(&m_uiMutex);
    m_freeFrames.push_back(frame);
    pthread_mutex_unlock(&m_uiMutex);
    /* Wake the engine out of a possible blocked-on-adopt park. */
    pthread_cond_signal(&m_cmdCond);
}

/* ==================================================================
 * Embedder entry points (UI thread). Each just queues a command (or, for
 * setDefaultCSS, sets a field) and returns immediately - the window never
 * blocks on a fetch/parse.
 * ================================================================== */

void EWebEngine::setViewport(int w, int h)
{
    EWebCmd cmd;
    cmd.kind = ECMD_RESIZE;
    cmd.x = w;
    cmd.y = h;
    postCommand(cmd);
}

void EWebEngine::setDefaultCSS(const char* url)
{
    /* Direct setter (not a command): call it before the first load, while the
     * engine is still parked, exactly like the original widget API. */
    m_defaultCSSUrl = EWebContainer::normalizeURL(&m_port, url != nullptr ? url : "", "");
}

void EWebEngine::setJSEnabled(bool enabled)
{
    EWebCmd cmd;
    cmd.kind = ECMD_SET_JS;
    cmd.b = enabled;
    postCommand(cmd);
}

void EWebEngine::load(const char* url)
{
    /* Posts ECMD_NAVIGATE and returns immediately; the engine aborts + cleans
     * up any in-flight page and starts the new load in its own context (see
     * engineNavigate). */
    if(url == nullptr || url[0] == 0)
        return;
    EWEB_LOG("[ewebview] queue html: %s\n", url);
    EWebCmd cmd;
    cmd.kind = ECMD_NAVIGATE;
    cmd.url = url;
    postCommand(cmd);
}

void EWebEngine::stop()
{
    /* It must never reach into engine state or block on a fetch/parse, so it
     * only posts ECMD_STOP and returns. The engine performs the actual
     * cleanup in its own context; postCommand() also raises the volatile
     * m_buildAbort so a long parse/script run unwinds at once. */
    EWebCmd cmd;
    cmd.kind = ECMD_STOP;
    postCommand(cmd);
}

void EWebEngine::reload()
{
    EWebCmd cmd;
    cmd.kind = ECMD_RELOAD;
    postCommand(cmd);
}

void EWebEngine::scrollTo(int x, int y)
{
    /* The embedder has already clamped (x,y) to the last-known doc geometry
     * and moved its own live offset; the engine adopts it as the
     * authoritative offset, re-renders the exposed strip and fires the page's
     * scroll handlers. */
    EWebCmd cmd;
    cmd.kind = ECMD_SCROLL;
    cmd.x = x;
    cmd.y = y;
    postCommand(cmd);
}

void EWebEngine::postInput(const eweb_event_t& ev)
{
    /* Asynchronous: the UI never blocks on the page's verdict, so a heavy
     * handler cannot freeze the window. */
    EWebCmd cmd;
    cmd.kind = ECMD_INPUT;
    cmd.ev = ev;
    cmd.x = ev.cx;
    cmd.y = ev.cy;
    postCommand(cmd);
}

void EWebEngine::postKey(const eweb_key_event_t& ev)
{
    /* Asynchronous, exactly like postInput: queue the gesture and let the
     * engine thread dispatch the DOM key events + default action there. */
    EWebCmd cmd;
    cmd.kind = ECMD_KEY;
    cmd.kev = ev;
    postCommand(cmd);
}

void EWebEngine::handleKeyDefault(const eweb_key_event_t& kev, const char* domKey, unsigned mods)
{
    /* ENGINE-THREAD ONLY. The built-in response to a key the page did not
     * cancel: Tab focus traversal, arrow-key navigation of an open <select>,
     * text editing on the focused field, Space/Enter activation of a focused
     * control, and page scrolling when nothing editable has focus. */
    (void)domKey; (void)mods;
    const bool shift = (kev.mods & EWEB_MOD_SHIFT) != 0;
    const bool ctrl  = (kev.mods & EWEB_MOD_CTRL)  != 0;
    const bool alt   = (kev.mods & EWEB_MOD_ALT)   != 0;
    /* Space reaches the engine either as EWEB_KEY_SPACE or - from the IME /
     * SDL_TEXTINPUT path, which is how the real frontend delivers a printable
     * key - as a CHAR carrying " ". Treat both as the space key for control
     * activation and page scrolling (a focused text field inserts it instead,
     * handled in its own branch below). */
    const bool space = (kev.key == EWEB_KEY_SPACE) ||
                       (kev.key == EWEB_KEY_CHAR && kev.text[0] == ' ' && kev.text[1] == 0);

    /* Tab / Shift+Tab cycles focus regardless of what currently has it. */
    if(kev.key == EWEB_KEY_TAB) { focusNext(shift); return; }

    /* An open <select> popup captures the keyboard until it is chosen/closed. */
    if(m_openSelect != nullptr) {
        eweb_el_input* sel = (eweb_el_input*)m_openSelect;
        switch(kev.key) {
        case EWEB_KEY_UP:   sel->moveOption(-1); markContentDirty(); return;
        case EWEB_KEY_DOWN: sel->moveOption(+1); markContentDirty(); return;
        case EWEB_KEY_ENTER:
            sel->chooseActiveOption();
            m_openSelect = nullptr;
            fireWidgetEvent((litehtml::element*)sel, "input");
            fireWidgetEvent((litehtml::element*)sel, "change");
            markContentDirty();
            return;
        case EWEB_KEY_ESCAPE:
            sel->toggleDropdown();   /* closes */
            m_openSelect = nullptr;
            markContentDirty();
            return;
        default: break;
        }
        if(space) {
            sel->chooseActiveOption();
            m_openSelect = nullptr;
            fireWidgetEvent((litehtml::element*)sel, "input");
            fireWidgetEvent((litehtml::element*)sel, "change");
            markContentDirty();
            return;
        }
    }

    litehtml::element* focusEl = (litehtml::element*)m_focusElement;
    eweb_el_input* w = (focusEl != nullptr) ? (eweb_el_input*)widgetAt(focusEl) : nullptr;

    /* Text editing on the focused field. */
    if(w != nullptr && w->isTextEditing()) {
        /* Ctrl+A arrives as the physical 'a' with CTRL from the frontend, or as
         * a CHAR "a" with CTRL from the injector; TEXTINPUT is suppressed for
         * command chords on the real path. */
        const bool ctrlA = ctrl && !alt &&
            (kev.key == 'a' || kev.key == 'A' ||
             (kev.key == EWEB_KEY_CHAR && (kev.text[0] == 'a' || kev.text[0] == 'A')));
        if(ctrlA) { w->selectAll(); markContentDirty(); return; }
        /* Ctrl+C / Ctrl+X / Ctrl+V ride the port's system clipboard
         * (sys.clipboard_set / sys.clipboard_get; OPTIONAL tables, so a port
         * without them leaves the chords inert). Like Ctrl+A they arrive as
         * the physical key with CTRL, or as a CHAR with CTRL from injection. */
        auto chord = [&](char c) {
            return ctrl && !alt &&
                (kev.key == c || kev.key == c - 32 ||
                 (kev.key == EWEB_KEY_CHAR &&
                  (kev.text[0] == c || kev.text[0] == c - 32)));
        };
        if(chord('c') || chord('x')) {
            if(m_port.sys.clipboard_set != nullptr) {
                std::string sel = w->selectedText();
                if(!sel.empty())
                    m_port.sys.clipboard_set(m_port.sys.ud, sel.c_str());
            }
            if(chord('x')) {
                w->deleteSelection();
                fireWidgetEvent(focusEl, "input");
                markContentDirty();
            }
            return;
        }
        if(chord('v')) {
            if(m_port.sys.clipboard_get != nullptr) {
                char* txt = m_port.sys.clipboard_get(m_port.sys.ud);
                if(txt != nullptr) {
                    w->insertText(txt);   /* replaces an active selection */
                    free(txt);            /* port contract: core releases it */
                    fireWidgetEvent(focusEl, "input");
                    markContentDirty();
                }
            }
            return;
        }
        bool edited = true;
        switch(kev.key) {
        case EWEB_KEY_CHAR:
            if(ctrl || alt) edited = false;   /* a shortcut chord, not text */
            else w->insertText(kev.text);
            break;
        case EWEB_KEY_BACKSPACE: w->deleteBack();    break;
        case EWEB_KEY_DELETE:    w->deleteForward(); break;
        case EWEB_KEY_LEFT:      w->moveCaret(-1, shift); edited = false; break;
        case EWEB_KEY_RIGHT:     w->moveCaret(+1, shift); edited = false; break;
        case EWEB_KEY_HOME:      w->moveCaret(-2, shift); edited = false; break;
        case EWEB_KEY_END:       w->moveCaret(+2, shift); edited = false; break;
        case EWEB_KEY_ENTER:
            if(w->inputType() == EWEB_INPUT_TEXTAREA) w->insertText("\n");
            else { submitForm(focusEl); return; }
            break;
        default: edited = false; break;
        }
        if(edited) fireWidgetEvent(focusEl, "input");
        markContentDirty();
        return;
    }

    /* Non-text form controls: Space/Enter activate, arrows adjust. */
    if(w != nullptr) {
        switch(w->inputType()) {
        case EWEB_INPUT_CHECKBOX:
            if(space) {
                w->keyActivate();
                fireWidgetEvent(focusEl, "input");
                fireWidgetEvent(focusEl, "change");
                markContentDirty(); return;
            }
            break;
        case EWEB_INPUT_RADIO:
            if(space && !w->isChecked()) {
                w->setChecked(true);
                clearRadioSiblings(w);
                fireWidgetEvent(focusEl, "input");
                fireWidgetEvent(focusEl, "change");
                markContentDirty(); return;
            }
            break;
        case EWEB_INPUT_SELECT:
            if(space || kev.key == EWEB_KEY_ENTER) {
                w->keyActivate();   /* opens the popup */
                m_openSelect = w->isDropdownOpen() ? (void*)w : nullptr;
                markContentDirty(); return;
            }
            if(kev.key == EWEB_KEY_UP || kev.key == EWEB_KEY_LEFT) {
                w->stepSelectedOption(-1);
                fireWidgetEvent(focusEl, "input");
                fireWidgetEvent(focusEl, "change");
                markContentDirty(); return;
            }
            if(kev.key == EWEB_KEY_DOWN || kev.key == EWEB_KEY_RIGHT) {
                w->stepSelectedOption(+1);
                fireWidgetEvent(focusEl, "input");
                fireWidgetEvent(focusEl, "change");
                markContentDirty(); return;
            }
            break;
        case EWEB_INPUT_RANGE:
            if(kev.key == EWEB_KEY_LEFT || kev.key == EWEB_KEY_DOWN) {
                w->stepRange(-1); fireWidgetEvent(focusEl, "input");
                markContentDirty(); return;
            }
            if(kev.key == EWEB_KEY_RIGHT || kev.key == EWEB_KEY_UP) {
                w->stepRange(+1); fireWidgetEvent(focusEl, "input");
                markContentDirty(); return;
            }
            break;
        case EWEB_INPUT_BUTTON:
            if(space || kev.key == EWEB_KEY_ENTER) {
                /* Keyboard activation fires a click, then the native action. */
                if(jsDispatchCancelableEvent(focusEl, "click", true))
                    activateWidget(w, 0, 0);
                return;
            }
            break;
        default: break;
        }
    }

    /* Escape drops focus (and any text selection). */
    if(kev.key == EWEB_KEY_ESCAPE) { clearFocus(); return; }

    /* Nothing editable focused: arrows / space / page keys scroll the document,
     * matching the browser default the embedder's wheel gesture also drives. */
    const int line = 24;
    const int page = (m_clientHeight > line) ? (m_clientHeight - line) : line;
    switch(kev.key) {
    case EWEB_KEY_DOWN:     scrollByKey(0, line);  return;
    case EWEB_KEY_UP:       scrollByKey(0, -line); return;
    case EWEB_KEY_RIGHT:    scrollByKey(line, 0);  return;
    case EWEB_KEY_LEFT:     scrollByKey(-line, 0); return;
    case EWEB_KEY_PAGEDOWN: scrollByKey(0, page);  return;
    case EWEB_KEY_PAGEUP:   scrollByKey(0, -page); return;
    case EWEB_KEY_HOME:     scrollByKey(0, -m_engineScrollY); return;
    case EWEB_KEY_END:      scrollByKey(0, m_doc ? m_doc->height() : 0); return;
    default: break;
    }
    if(space) { scrollByKey(0, shift ? -page : page); return; }
}

/* Collect the focusable elements (form widgets + <a href>) under `e` in
 * document order. Widgets are recognised through the litehtml hook so a
 * disabled/hidden control is skipped; anchors are matched by tag + href. */
static void collectFocusable(litehtml::element* e, std::vector<litehtml::element*>& out)
{
    if(e == nullptr) return;
    void* w = e->eweb_form_widget();
    if(w != nullptr) {
        if(((eweb_el_input*)w)->isFocusable()) out.push_back(e);
    } else {
        const litehtml::tchar_t* tag = e->get_tagName();
        const litehtml::tchar_t* href = e->get_attr("href", nullptr);
        if(tag != nullptr && tag[0] == 'a' && tag[1] == 0 &&
                href != nullptr && href[0] != 0)
            out.push_back(e);
    }
    size_t n = e->get_children_count();
    for(size_t i = 0; i < n; ++i) {
        litehtml::element::ptr c = e->get_child((int)i);
        collectFocusable(c, out);
    }
}

void* EWebEngine::widgetAt(litehtml::element* el) const
{
    /* Walk the hit element and its ancestors: a click on a <button>'s text
     * child must still resolve to the control. */
    for(litehtml::element* e = el; e != nullptr; e = e->parent()) {
        void* w = e->eweb_form_widget();
        if(w != nullptr) return w;
    }
    return nullptr;
}

litehtml::element* EWebEngine::hitElementAt(int cx, int cy)
{
    /* The hit tester works in unscrolled document coordinates, so add the
     * engine's scroll offsets; the client pair stays as-is for position:fixed. */
    litehtml::document* doc = jsActiveDoc();
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    litehtml::element::ptr t =
        root->get_element_by_point(cx + m_engineScrollX, cy + m_engineScrollY, cx, cy);
    return t;
}

litehtml::element* EWebEngine::focusableAt(litehtml::element* el) const
{
    for(litehtml::element* e = el; e != nullptr; e = e->parent()) {
        void* w = e->eweb_form_widget();
        if(w != nullptr)
            return ((eweb_el_input*)w)->isFocusable() ? e : nullptr;
        const litehtml::tchar_t* tag = e->get_tagName();
        const litehtml::tchar_t* href = e->get_attr("href", nullptr);
        if(tag != nullptr && tag[0] == 'a' && tag[1] == 0 &&
                href != nullptr && href[0] != 0)
            return e;
    }
    return nullptr;
}

void EWebEngine::setFocus(litehtml::element* el)
{
    /* ENGINE-THREAD ONLY. Move keyboard focus, firing blur/focusout on the old
     * element and focus/focusin on the new one (the DOM order: the outgoing
     * element loses focus first). focus/blur do not bubble; focusin/focusout
     * do. The widget's own focus flag drives the caret / focus ring in draw(). */
    litehtml::element* old = (litehtml::element*)m_focusElement;
    if(old == el) return;

    if(old != nullptr) {
        void* w = widgetAt(old);
        if(w != nullptr) ((eweb_el_input*)w)->setFocused(false);
        jsDispatchSimpleEvent(old, "blur", false);
        jsDispatchSimpleEvent(old, "focusout", true);
    }
    m_focusElement = (void*)el;
    if(el != nullptr) {
        void* w = widgetAt(el);
        if(w != nullptr) ((eweb_el_input*)w)->setFocused(true);
        jsDispatchSimpleEvent(el, "focus", false);
        jsDispatchSimpleEvent(el, "focusin", true);
    }
    /* Repaint so the caret / focus ring appears or disappears. */
    markContentDirty();
}

void EWebEngine::clearFocus()
{
    setFocus(nullptr);
}

void EWebEngine::focusNext(bool reverse)
{
    /* ENGINE-THREAD ONLY (Tab / Shift+Tab). Cycle through the focusable
     * elements in document order, wrapping at the ends. With nothing focused
     * the first (or last, when reversing) focusable takes focus. */
    litehtml::document* doc = jsActiveDoc();
    if(doc == nullptr) return;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return;
    std::vector<litehtml::element*> list;
    collectFocusable(root, list);
    if(list.empty()) { clearFocus(); return; }

    litehtml::element* cur = (litehtml::element*)m_focusElement;
    int idx = -1;
    for(size_t i = 0; i < list.size(); ++i)
        if(list[i] == cur) { idx = (int)i; break; }

    int n = (int)list.size();
    int next;
    if(idx < 0)      next = reverse ? n - 1 : 0;
    else if(reverse) next = (idx - 1 + n) % n;
    else             next = (idx + 1) % n;
    setFocus(list[next]);
}

void EWebEngine::fireWidgetEvent(litehtml::element* el, const char* type)
{
    /* input/change bubble and are not cancelable (matching the DOM), so the
     * simple dispatch is exactly right. */
    jsDispatchSimpleEvent(el, type, true);
}

void EWebEngine::clearRadioSiblings(void* widget)
{
    /* A radio button belongs to the group of same-name radios in its tree; turn
     * the others off so only the newly-checked one stays selected. */
    if(widget == nullptr) return;
    eweb_el_input* w = (eweb_el_input*)widget;
    litehtml::element* wel = (litehtml::element*)w;
    const litehtml::tchar_t* name = wel->get_attr(_t("name"));
    if(name == nullptr || name[0] == 0) return;   /* unnamed radios form no group */
    litehtml::document* doc = jsActiveDoc();
    if(doc == nullptr) return;
    std::vector<eweb_el_input*> all;
    collectWidgets(doc->root(), all);
    for(size_t i = 0; i < all.size(); ++i) {
        eweb_el_input* o = all[i];
        if(o == w || o->inputType() != EWEB_INPUT_RADIO) continue;
        const litehtml::tchar_t* on = ((litehtml::element*)o)->get_attr(_t("name"));
        if(on != nullptr && !t_strcasecmp(on, name)) o->setChecked(false);
    }
}

void EWebEngine::activateWidget(void* widget, int localX, int localY)
{
    /* ENGINE-THREAD ONLY. The native activation for a control the user clicked
     * (or activated by key). localX/localY are the control's border-box local
     * coords. Text caret/editing is driven by the press + key paths instead. */
    if(widget == nullptr) return;
    eweb_el_input* w = (eweb_el_input*)widget;
    litehtml::element* el = (litehtml::element*)w;
    switch(w->inputType()) {
    case EWEB_INPUT_CHECKBOX:
        w->activate(localX, localY);   /* toggles */
        fireWidgetEvent(el, "input");
        fireWidgetEvent(el, "change");
        break;
    case EWEB_INPUT_RADIO:
        if(!w->isChecked()) {
            w->setChecked(true);
            clearRadioSiblings(w);
            fireWidgetEvent(el, "input");
            fireWidgetEvent(el, "change");
        }
        break;
    case EWEB_INPUT_SELECT:
        /* Only one popup is open at a time. */
        if(m_openSelect != nullptr && m_openSelect != widget)
            ((eweb_el_input*)m_openSelect)->toggleDropdown();
        w->toggleDropdown();
        m_openSelect = w->isDropdownOpen() ? widget : nullptr;
        break;
    case EWEB_INPUT_RANGE:
        w->setRangeFromX(localX);
        fireWidgetEvent(el, "input");
        break;
    case EWEB_INPUT_BUTTON: {
        /* <input type=submit> and a <button> with no type submit their form;
         * type=button/reset have no native navigation here. */
        const litehtml::tchar_t* ty = el->get_attr(_t("type"));
        const litehtml::tchar_t* tag = el->get_tagName();
        bool isSubmit;
        if(ty != nullptr && ty[0] != 0) isSubmit = !t_strcasecmp(ty, _t("submit"));
        else isSubmit = (tag != nullptr && !t_strcasecmp(tag, _t("button")));
        if(isSubmit) submitForm(el);
        break;
    }
    default:
        break;   /* text/hidden: nothing to activate on click */
    }
    markContentDirty();
}

void EWebEngine::submitForm(litehtml::element* field)
{
    if(field == nullptr) return;
    /* Walk up to the enclosing <form>; a control with no form does nothing. */
    litehtml::element* form = nullptr;
    for(litehtml::element* e = field; e != nullptr; e = e->parent()) {
        const litehtml::tchar_t* tag = e->get_tagName();
        if(tag != nullptr && !t_strcasecmp(tag, _t("form"))) { form = e; break; }
    }
    if(form == nullptr) return;

    /* A submit listener may preventDefault() (e.g. to run an XHR submit). */
    if(!jsDispatchCancelableEvent(form, "submit", true)) return;

    const litehtml::tchar_t* actionA = form->get_attr(_t("action"));
    std::string action = (actionA != nullptr && actionA[0] != 0)
                         ? std::string(actionA) : m_currentHtmlUrl;
    /* Drop any existing query/fragment before appending the new one. */
    size_t cut = action.find_first_of("?#");
    if(cut != std::string::npos) action = action.substr(0, cut);

    /* Collect the successful controls: named, not disabled, checked for
     * checkbox/radio, buttons excluded. */
    std::vector<eweb_el_input*> all;
    collectWidgets(form, all);
    std::string query;
    for(size_t i = 0; i < all.size(); ++i) {
        eweb_el_input* w = all[i];
        litehtml::element* we = (litehtml::element*)w;
        if(we->get_attr(_t("disabled")) != nullptr) continue;
        const litehtml::tchar_t* nameA = we->get_attr(_t("name"));
        if(nameA == nullptr || nameA[0] == 0) continue;
        EWebInputType t = w->inputType();
        if(t == EWEB_INPUT_BUTTON) continue;
        if((t == EWEB_INPUT_CHECKBOX || t == EWEB_INPUT_RADIO) && !w->isChecked()) continue;
        if(!query.empty()) query += "&";
        query += eweb_urlencode(nameA);
        query += "=";
        query += eweb_urlencode(w->value().c_str());
    }

    std::string url = action;
    if(!query.empty()) { url += "?"; url += query; }
    /* NOTE: only GET navigation is supported; a method=post form has no POST
     * navigation channel here, so it is submitted as GET (documented limit). */
    queueNavigation(url);
}

bool EWebEngine::selectPopupRect(litehtml::position& out, int& rowH, int& visibleRows)
{
    out = litehtml::position(0, 0, 0, 0);
    rowH = 0;
    visibleRows = 0;
    if(m_openSelect == nullptr) return false;
    eweb_el_input* w = (eweb_el_input*)m_openSelect;
    if(!w->isDropdownOpen()) return false;
    int rows = w->dropdownRowCount();
    if(rows <= 0) return false;
    const int maxRows = 8;
    visibleRows = (rows < maxRows) ? rows : maxRows;
    rowH = w->popupRowHeight();

    litehtml::position pl = ((litehtml::element*)w)->get_placement();
    int px = pl.x - m_engineScrollX;
    int py = pl.y - m_engineScrollY + pl.height;   /* just below the control */
    int h = visibleRows * rowH + 2;
    /* Flip above the control when it would overflow the viewport bottom. */
    if(py + h > m_clientHeight) py = pl.y - m_engineScrollY - h;
    if(py < 0) py = 0;
    out.x = px; out.y = py; out.width = pl.width; out.height = h;
    return true;
}

void EWebEngine::drawSelectPopup(eweb_surface_t* cache)
{
    litehtml::position r; int rowH = 0, visible = 0;
    if(!selectPopupRect(r, rowH, visible)) return;
    ((eweb_el_input*)m_openSelect)->drawDropdown(cache, r.x, r.y, r.width, rowH, visible);
}

void EWebEngine::scrollByKey(int dx, int dy)
{
    /* ENGINE-THREAD ONLY. Adopt the stepped offset, repaint, fire the page's
     * scroll handlers and republish the authoritative offset to the embedder
     * (postScrollClamp clamps to the doc geometry and snaps the UI's offset). */
    m_engineScrollX += dx;
    m_engineScrollY += dy;
    markContentDirty();
    postScrollClamp();
    jsFireScrollEvent();
}

void EWebEngine::cleanupBuildResources()
{
    /* Tear down any JS VM from the previous page so its globals and the
     * DOM-bridge element handles do not outlive the document they point into;
     * vm_close() frees the VM and resetJsVm() nulls the pointer. Also drop
     * the extracted scripts and reset the document.write reparse guard. */
    resetJsVm();
    m_jsScripts.clear();
    m_jsScriptSrcs.clear();
    m_jsScriptDone.clear();
    m_jsHasInlineHandlers = false;
    m_jsReparseCount = 0;
    m_jsRunBeforePaint = false;
    m_jsNextScript = 0;
    m_jsScriptWaitSince = 0;
    m_jsPostSwapRun = false;
    m_jsProgressiveActive = false;
    m_jsLastFlushAt = 0;
    m_jsFlushCostMs = 0;
    m_jsInScript = false;
    m_jsEnterAt = 0;
    m_jsAbortCount = 0;
    m_jsPageDisabled = false;
    m_jsMutations.clear();
    /* Nodes removeChild() parked belong to the page being torn down, so they
     * go with it. Cookies (process-wide EWebCookieJar) and the two Web
     * Storage blobs deliberately SURVIVE: they are not VM state, so
     * document.cookie and localStorage persist across a navigation exactly as
     * they do in a browser. The deferred navigation/scroll requests die with
     * the page they were made from - load() is what calls us, so acting on
     * them again would loop. */
    jsFreeDetachedNodes();
    m_jsPendingNav.clear();
    m_jsScrollPending = false;
    /* Drop every handle that points into the page being torn down: the hover,
     * focus and drag elements are litehtml::element* and m_openSelect /
     * m_dragWidget are eweb_el_input* owned by that page's container, so they
     * all dangle once the document dies. Leaving them set would hand a stale
     * pointer to the next dispatch. */
    m_jsHoverElement = nullptr;
    m_focusElement = nullptr;
    m_openSelect = nullptr;
    m_rangeDragging = false;
    m_textDragging = false;
    m_dragWidget = nullptr;
    /* Drop canvas backing stores with the VM that references them (resetJsVm
     * above already freed the ctx objects holding these EWebCanvas
     * pointers). */
    freeCanvases();
    m_buildHtmlContent.clear();
    m_buildHtmlUrl.clear();
    m_buildNeedsStyleUpdate = false;
    m_buildNeedsLayout = false;
    m_needsLayout = false;
    m_needsStyleUpdate = false;
    m_styleNeedSince = 0;
    m_flushDeferredImages = false;
    m_defaultCssPrepared = false;
    m_defaultCssLoading = false;
    m_deferBuildStep = false;
    m_styleStepInFlight = false;
    m_layoutDirtyAt = 0;
    m_buildLayoutDirtyAt = 0;
    m_layoutDirtySince = 0;
    m_buildLayoutDirtySince = 0;
    m_seenCssUrls.clear();
    m_buildPhase = BUILD_IDLE;
    /* The abort (if any) is what brought us here: consumed. */
    m_buildAbort = false;
    m_buildTargetContext = nullptr;
    if(m_buildDoc) {
        delete m_buildDoc;
        m_buildDoc = nullptr;
    }
    if(m_buildContainer) {
        delete m_buildContainer;
        m_buildContainer = nullptr;
    }
    /* Clear the embedder's build overlay by posting an empty status
     * (m_buildPhase is BUILD_IDLE by this point, so the overlay flag goes
     * false with it). */
    setBuildStatus("", 0);
}

void EWebEngine::setBuildStatus(const std::string& status, int progress)
{
    /* ENGINE-THREAD ONLY. The status text, progress and the overlay flag are
     * mirrored to the UI thread via EUET_BUILD_STATUS (drained in tick(),
     * which fires the on_status/on_build_status hooks); the engine keeps no
     * copy. The overlay is suppressed during a post-swap script run
     * (m_jsPostSwapRun) so the page stays visible while its scripts execute -
     * the status text still reaches the status bar. */
    EWebUiEvent ev;
    ev.kind = EUET_BUILD_STATUS;
    ev.text = status;
    ev.progress = progress;
    ev.w = (m_buildPhase != BUILD_IDLE && !m_jsPostSwapRun) ? 1 : 0;
    postUiEvent(ev);
}

void EWebEngine::clampScrollLocked(int docWidth, int docHeight)
{
    /* ENGINE-THREAD ONLY: clamps the engine's authoritative scroll offset to
     * the document geometry and the client viewport. Uses m_clientWidth/
     * Height (the engine's own copy of the viewport size, kept in step by
     * ECMD_RESIZE). The "Locked" suffix is legacy: the engine exclusively
     * owns this state now, so no mutex is involved. */
    int maxX = docWidth - m_clientWidth;
    int maxY = docHeight - m_clientHeight;
    if(maxX < 0) {
        maxX = 0;
    }
    if(maxY < 0) {
        maxY = 0;
    }
    if(m_engineScrollX < 0) {
        m_engineScrollX = 0;
    } else if(m_engineScrollX > maxX) {
        m_engineScrollX = maxX;
    }
    if(m_engineScrollY < 0) {
        m_engineScrollY = 0;
    } else if(m_engineScrollY > maxY) {
        m_engineScrollY = maxY;
    }
}

bool EWebEngine::hasSeenCSS(const std::string& url) const
{
    for(const auto& seen_url : m_seenCssUrls) {
        if(seen_url == url) {
            return true;
        }
    }
    return false;
}

void EWebEngine::rememberCSS(const std::string& url)
{
    if(url.empty() || hasSeenCSS(url)) {
        return;
    }
    m_seenCssUrls.push_back(url);
}

void EWebEngine::forgetCSS(const std::string& url)
{
    for(size_t i = 0; i < m_seenCssUrls.size(); ++i) {
        if(m_seenCssUrls[i] == url) {
            m_seenCssUrls.erase(m_seenCssUrls.begin() + i);
            break;
        }
    }
}

/* ==================================================================
 * EWebContainerHost (engine thread, called from litehtml callbacks)
 * ================================================================== */

bool EWebEngine::queueImageTask(const std::string& url)
{
    /* EWebContainer::load_image / flushPendingImages -> here. Queue an async
     * image fetch; duplicates are dropped by addTask. */
    EWebTask task;
    task.url = url;
    task.type = EWEB_TASK_IMAGE;
    task.loading = false;
    return addTask(task);
}

void EWebEngine::loadCSS(const std::string& url)
{
    std::string full_url = EWebContainer::normalizeURL(&m_port, url, "");
    if(full_url.empty()) {
        return;
    }
    if(hasSeenCSS(full_url)) {
        return;
    }
    rememberCSS(full_url);
    m_pendingCss++;
    EWebTask task;
    task.url = full_url;
    task.type = EWEB_TASK_CSS;
    task.loading = false;
    if(!addTask(task)) {
        forgetCSS(full_url);
        m_pendingCss--;
        return;
    }
}

void EWebEngine::setCSSMedia(const std::string& url, const std::string& media)
{
    /* Runs on the engine thread during createFromString (el_link ->
     * EWebContainer::link -> here). m_cssMediaMutex guards the url->media
     * map; it is a dedicated lock (never held across document work) so this
     * callback stays safe regardless of what else the create path holds. */
    pthread_mutex_lock(&m_cssMediaMutex);
    if(media.empty()) {
        m_cssMedia.erase(url);
    } else {
        m_cssMedia[url] = media;
    }
    pthread_mutex_unlock(&m_cssMediaMutex);
}

void EWebEngine::queueNavigation(const std::string& url)
{
    if(url.empty())
        return;
    /* ENGINE-THREAD ONLY: an <a href> click (EWebContainer::on_anchor_click)
     * or a script's location.href routes here. Resolve the target against the
     * page on screen and record it as pending; the engine loop acts on it
     * (abort + cleanup + load) once the current run unwinds - see
     * jsRunPendingNavigation. No locking: the engine exclusively owns this
     * state and the documents. */
    std::string full = EWebContainer::normalizeURL(&m_port, url, m_currentHtmlUrl);
    if(full.empty())
        full = url;
    m_jsPendingNav = full;
    EWEB_LOG("[ewebview] anchor navigation queued -> %s\n", full.c_str());
    /* Abort the in-flight build/script run so this navigation starts on the
     * next loop iteration instead of after seconds of dead parsing or
     * leftover scripts. No-op when nothing is in flight. */
    requestBuildAbort();
}

void EWebEngine::requestBuildAbort()
{
    /* ENGINE-THREAD ONLY. Raises the abort flags so the current
     * createFromString/render/VM run unwinds fast: m_buildAbort (also raised
     * from the UI thread by postCommand as a cross-thread signal) is what
     * jsOnVmStep and the EWebContainer hot callbacks poll, and the build
     * container's own short-circuit flag makes litehtml's callbacks bail. The
     * engine loop then discards the half-built page. Never called from the UI
     * thread (the UI signals via postCommand instead). */
    m_buildAbort = true;
    if(m_buildContainer)
        m_buildContainer->setAbort(true);
    EWEB_LOG("[ewebview] build abort requested (phase=%d)\n", (int)m_buildPhase);
}

/* ==================================================================
 * Download worker thread
 * ================================================================== */

static void* _ew_task_thread(void* p)
{
    EWebEngine* engine = (EWebEngine*)p;
    if(engine != nullptr)
        engine->taskLoop();
    return nullptr;
}

void EWebEngine::taskLoop()
{
    /* Download worker body (one on-demand thread, spawned by addTask and
     * reaped when the queue drains). Only touches the mutex-guarded
     * task/result queues and the port's fetch/decode callbacks - never the
     * documents. */
    EWebTask task;
    bool havetask = false;
    while(!m_task_ended) {
        if(getTask(task)) {
            havetask = true;
            bool res = false;
            {   /* Report on the UI thread: the worker must never call the
                 * listener hooks itself - it posts an event the UI drains in
                 * tick(), where the hook finally fires. */
                EWebUiEvent sev; sev.kind = EUET_TASK_START; sev.task = task;
                postUiEvent(sev);
            }
            // Process task
            if(task.type == EWEB_TASK_HTML) {
                res = loadHtmlTask(task.url);
            } else if(task.type == EWEB_TASK_CSS) {
                res = loadCSSTask(task.url);
            } else if(task.type == EWEB_TASK_IMAGE) {
                res = loadImageTask(task.url);
            } else if(task.type == EWEB_TASK_SCRIPT) {
                res = loadScriptTask(task.url);
            }

            EWebUiEvent sev; sev.task = task;
            sev.kind = res ? EUET_TASK_END : EUET_TASK_FAILED;
            postUiEvent(sev);
        }
        else {
            // No task left: notify once, then tear down the worker.
            // addTask() already recreates the thread on demand, so keeping
            // an idle worker parked only exposes a fragile sleep/restore
            // path for detached child threads.
            if(havetask) {
                havetask = false;
                EWEB_LOG("[ewebview] task thread idle: queue drained\n");
                EWebUiEvent sev; sev.kind = EUET_TASKS_END;
                postUiEvent(sev);
            }
            pthread_mutex_lock(&m_taskMutex);
            bool queue_empty = m_taskQueue.empty();
            if(queue_empty) {
                m_task_running = false;
            }
            pthread_mutex_unlock(&m_taskMutex);
            if(queue_empty) {
                return;
            }
        }
    }

    m_task_running = false;
}

bool EWebEngine::addTask(const EWebTask& task)
{
    pthread_mutex_lock(&m_taskMutex);

    for(auto& t : m_taskQueue) {
        if(t.url == task.url) {
            if(task.type == EWEB_TASK_IMAGE) {
                EWEB_LOG("[ewebview] queue image skipped: duplicate loading=%d queue=%d running=%d\n",
                    t.loading ? 1 : 0, (int)m_taskQueue.size(), m_task_running ? 1 : 0);
            }
            pthread_mutex_unlock(&m_taskMutex);
            return false;
        }
    }

    m_taskQueue.push_back(task);
    if(task.type == EWEB_TASK_IMAGE) {
        EWEB_LOG("[ewebview] queue image added: pending=%d running=%d\n",
            (int)m_taskQueue.size(), m_task_running ? 1 : 0);
    }
    pthread_mutex_unlock(&m_taskMutex);

    if(!m_task_running) {
        pthread_t tid;
        m_task_running = true;
        if(pthread_create(&tid, NULL, _ew_task_thread, this) != 0) {
            m_task_running = false;
            return false;
        }
        pthread_detach(tid);
    }
    return true;
}

void EWebEngine::removeTask(const std::string& url)
{
    pthread_mutex_lock(&m_taskMutex);
    for(size_t i = 0; i < m_taskQueue.size(); i++) {
        if(m_taskQueue[i].url == url) {
            m_taskQueue.erase(m_taskQueue.begin() + i);
            break;
        }
    }
    pthread_mutex_unlock(&m_taskMutex);
}

/* The download worker thread asks for these on every fetch, so both sides go
 * through m_taskMutex; a copy is returned because the caller then hands it to
 * EWebContainer::loadURL outside any lock. */
std::string EWebEngine::taskPageUrl()
{
    pthread_mutex_lock(&m_taskMutex);
    std::string url = m_taskPageUrl;
    pthread_mutex_unlock(&m_taskMutex);
    return url;
}

void EWebEngine::setTaskPageUrl(const std::string& url)
{
    pthread_mutex_lock(&m_taskMutex);
    m_taskPageUrl = url;
    pthread_mutex_unlock(&m_taskMutex);
}

bool EWebEngine::getTask(EWebTask& task)
{
    pthread_mutex_lock(&m_taskMutex);

    // Check if we should exit
    if(m_task_ended && m_taskQueue.empty()) {
        pthread_mutex_unlock(&m_taskMutex);
        return false;
    }

    // Get task from queue
    for(size_t i = 0; i < m_taskQueue.size(); i++) {
        if(!m_taskQueue[i].loading) {
            task = m_taskQueue[i];
            m_taskQueue[i].loading = true;
            if(task.type == EWEB_TASK_IMAGE) {
                EWEB_LOG("[ewebview] take image task: queue=%d\n",
                    (int)m_taskQueue.size());
            }
            pthread_mutex_unlock(&m_taskMutex);
            return true;
        }
    }

    pthread_mutex_unlock(&m_taskMutex);
    return false;
}

bool EWebEngine::loadHtmlTask(const std::string& url)
{
    EWebResult result = {url, EWEB_TASK_HTML, false, "", nullptr};
    int sz = 0;
    uint64_t fetch_start = ticMs();
    /* Top-level navigation: SameSite=Lax cookies may ride along even when the
     * initiator is another site, Strict ones may not. */
    uint8_t* content = EWebContainer::loadURL(&m_port, url, &sz, taskPageUrl(), true);
    if(content != NULL) {
        if(sz > 0)
            result.content.assign((char*)content, sz);
        else
            result.content = (char*)content;
        free(content);
        result.ok = true;
    }
    EWEB_LOG("[ewebview] fetched html: url=%s ok=%d size=%d cost=%u ms\n",
        url.c_str(), result.ok ? 1 : 0, sz, (uint32_t)(ticMs() - fetch_start));
    pushResult(result);
    removeTask(url);
    return result.ok;
}

bool EWebEngine::loadCSSTask(const std::string& url)
{
    EWebResult result = {url, EWEB_TASK_CSS, false, "", nullptr};
    int sz = 0;
    uint64_t fetch_start = ticMs();
    /* Subresource of the document being built: not a top-level navigation, so
     * Lax/Strict cookies both stay home when the sheet comes from another
     * site. */
    uint8_t* content = EWebContainer::loadURL(&m_port, url, &sz, taskPageUrl(), false);
    if(content != NULL) {
        if(sz > 0)
            result.content.assign((char*)content, sz);
        else
            result.content = (char*)content;
        free(content);
        result.ok = true;
    }
    pushResult(result);
    removeTask(url);
    EWEB_LOG("[ewebview] fetched css: url=%s ok=%d size=%d cost=%u ms\n",
        url.c_str(), result.ok ? 1 : 0, sz, (uint32_t)(ticMs() - fetch_start));
    return result.ok;
}

bool EWebEngine::loadScriptTask(const std::string& url)
{
    EWebResult result = {url, EWEB_TASK_SCRIPT, false, "", nullptr};
    int sz = 0;
    uint64_t fetch_start = ticMs();
    /* Subresource of the document being built (a classic <script src>): not a
     * top-level navigation, so Lax/Strict cookies both stay home cross-site. */
    uint8_t* content = EWebContainer::loadURL(&m_port, url, &sz, taskPageUrl(), false);
    if(content != NULL) {
        if(sz > 0)
            result.content.assign((char*)content, sz);
        else
            result.content = (char*)content;
        free(content);
        result.ok = true;
    }
    pushResult(result);
    removeTask(url);
    EWEB_LOG("[ewebview] fetched script: url=%s ok=%d size=%d cost=%u ms\n",
        url.c_str(), result.ok ? 1 : 0, sz, (uint32_t)(ticMs() - fetch_start));
    return result.ok;
}

bool EWebEngine::loadImageTask(const std::string& url)
{
    EWebResult result = {url, EWEB_TASK_IMAGE, false, "", nullptr};
    int sz = 0;
    uint64_t fetch_start = ticMs();
    /* Subresource, same reasoning as loadCSSTask(). */
    uint8_t* content = EWebContainer::loadURL(&m_port, url, &sz, taskPageUrl(), false);
    if(content != NULL && sz > 0) {
        /* Decode on this worker thread (pure heap work): the engine thread
         * then only mounts the finished surface in O(1) instead of burning
         * tens of ms per image. */
        uint64_t decode_start = ticMs();
        result.image = EWebContainer::decodeImageData(&m_port, content, sz);
        EWEB_LOG("[ewebview] worker decode image: url=%s hash=%08x ok=%d size=%d cost=%u ms\n",
            url.c_str(), debug_hash_text(url), result.image ? 1 : 0, sz,
            (uint32_t)(ticMs() - decode_start));
        free(content);
        result.ok = true;
    }
    EWEB_LOG("[ewebview] image result ready: url=%s hash=%08x ok=%d size=%d\n",
        url.c_str(), debug_hash_text(url), result.ok ? 1 : 0, sz);
    pushResult(result);
    removeTask(url);
    EWEB_LOG("[ewebview] fetched image: ok=%d size=%d cost=%u ms\n",
        result.ok ? 1 : 0, sz, (uint32_t)(ticMs() - fetch_start));
    return result.ok;
}

bool EWebEngine::loadCSSContent(const std::string& url, const std::string& content)
{
    /* ENGINE-THREAD ONLY (called from processResults). The engine exclusively
     * owns the documents and contexts: parsed CSS marks the target doc
     * style/layout-dirty and the engine loop re-renders a frame. */
    if(m_pendingCss > 0) {
        m_pendingCss--;
    }
    bool res = false;
    if(!content.empty()) {
        uint64_t parse_start = ticMs();
        litehtml::context* ctx = m_activeContext;
        litehtml::document::ptr target_doc = m_doc;
        bool target_build = false;
        /* During a post-swap script run the visible doc is the live target;
         * only a real build (doc not yet swapped) redirects CSS to the build
         * context. */
        if((m_buildPhase != BUILD_IDLE && !m_jsPostSwapRun) || m_buildDoc != nullptr || m_defaultCssLoading) {
            ctx = m_buildTargetContext ? m_buildTargetContext : &m_buildContext;
            target_doc = m_buildDoc;
            target_build = true;
        }
        /* An in-flight chunked style update is stale once new master css
         * arrives: its epoch stamps would skip elements against the old
         * stylesheet set, so restart it from scratch. */
        if(target_doc) {
            target_doc->abort_style_step();
        }
        /* A <link media="..."> sheet whose media never matches this device
         * must not reach the master stylesheet: master selectors carry no
         * media list, so print-only rules would render on screen. */
        bool media_matches = true;
        std::string media_attr;
        pthread_mutex_lock(&m_cssMediaMutex);
        std::unordered_map<std::string, std::string>::iterator mit = m_cssMedia.find(url);
        if(mit != m_cssMedia.end())
            media_attr = mit->second;
        pthread_mutex_unlock(&m_cssMediaMutex);
        if(!media_attr.empty()) {
            litehtml::media_query_list::ptr mlist =
                litehtml::media_query_list::create_from_string(media_attr.c_str(), nullptr);
            if(mlist) {
                EWebContainer* media_cont = target_build ? m_buildContainer : m_container;
                if(media_cont) {
                    litehtml::media_features feat;
                    media_cont->get_media_features(feat);
                    mlist->apply_media_features(feat);
                    media_matches = mlist->is_used();
                }
            }
        }
        if(media_matches) {
            ctx->load_master_stylesheet(content.c_str());
        } else {
            EWEB_LOG("[ewebview] skip css (media mismatch): url=%s media=%s\n",
                url.c_str(), media_attr.c_str());
        }
        uint32_t parse_ms = (uint32_t)(ticMs() - parse_start);
        bool is_default_css = (!m_defaultCSSUrl.empty() && url == m_defaultCSSUrl);
        if(is_default_css) {
            m_defaultCssPrepared = true;
            m_defaultCssLoading = false;
        }
        EWEB_LOG("[ewebview] parse css: url=%s size=%d cost=%u ms\n", url.c_str(), (int)content.size(), parse_ms);
        if(target_doc) {
            if(target_build) {
                m_buildNeedsStyleUpdate = true;
            } else {
                if(!m_needsStyleUpdate)
                    m_styleNeedSince = ticMs();
                m_needsStyleUpdate = true;
            }
            markLayoutDirty(target_build);
            res = true;
        } else if(is_default_css) {
            if(m_buildPhase == BUILD_PRELOAD_CSS) {
                m_buildPhase = BUILD_CREATE_DOC;
            }
            res = true;
        }
    }
    return res;
}

bool EWebEngine::loadImageContent(const std::string& url, uint8_t* content, int sz)
{
    /* ENGINE-THREAD ONLY (called from processResults). The decoded bytes go
     * to the owning container's image cache and layout is marked dirty; the
     * engine loop re-renders a frame. */
    bool res = false;
    if(m_doc == nullptr && m_buildDoc == nullptr) {
        EWEB_LOG("[ewebview] image content deferred: no-doc url=%s hash=%08x size=%d\n",
            url.c_str(), debug_hash_text(url), sz);
        return false;
    }
    EWebContainer* target_container = m_container;
    litehtml::document::ptr target_doc = m_doc;
    bool target_build = false;
    if(m_buildDoc != nullptr && m_buildContainer != NULL) {
        target_container = m_buildContainer;
        target_doc = m_buildDoc;
        target_build = true;
    }
    EWEB_LOG("[ewebview] image content target: url=%s hash=%08x build=%d doc=%p container=%p size=%d\n",
        url.c_str(), debug_hash_text(url), target_build ? 1 : 0, (void*)target_doc, (void*)target_container, sz);
    if(content != NULL && sz > 0 && target_container != NULL && target_doc != nullptr) {
        uint64_t decode_start = ticMs();
        res = target_container->loadImageData(url, content, sz);
        uint32_t decode_ms = (uint32_t)(ticMs() - decode_start);
        EWEB_LOG("[ewebview] decode image: ok=%d size=%d cost=%u ms build=%d\n",
            res ? 1 : 0, sz, decode_ms, target_build ? 1 : 0);
        if(res && target_doc) {
            /* The bitmap is in the cache now, but the element was measured
             * without it: layout has to run again or the image stays 0x0. */
            markLayoutDirty(target_build);
        }
    }
    if(!res)
        EWEB_LOG("[ewebview] image content failed: url=%s hash=%08x size=%d\n",
            url.c_str(), debug_hash_text(url), sz);
    return res;
}

bool EWebEngine::mountDecodedImage(const std::string& url, eweb_surface_t* img)
{
    /* ENGINE-THREAD half of the worker-side decode: cache the finished
     * surface and mark layout dirty. O(1) - no decoding, no allocation-heavy
     * work here. */
    bool res = false;
    if(m_doc == nullptr && m_buildDoc == nullptr) {
        EWEB_LOG("[ewebview] image mount deferred: no-doc url=%s hash=%08x\n",
            url.c_str(), debug_hash_text(url));
        return false;
    }
    EWebContainer* target_container = m_container;
    bool target_build = false;
    if(m_buildDoc != nullptr && m_buildContainer != NULL) {
        target_container = m_buildContainer;
        target_build = true;
    }
    if(img != NULL && target_container != NULL) {
        res = target_container->mountImage(url, img);
        if(res) {
            markLayoutDirty(target_build);
        }
    }
    return res;
}

bool EWebEngine::loadHtmlContent(const std::string& content)
{
    /* ENGINE-THREAD ONLY (called from processResults after the HTML fetch
     * lands). It resets the build state, strips <script> bodies, picks the
     * target context and kicks the build state machine. The engine
     * exclusively owns the documents; the loop renders frames and posts them
     * to the UI. */
    cleanupBuildResources();
    m_buildTargetContext = (m_activeContext == &m_browser_context) ? &m_buildContext : &m_browser_context;
    m_buildTargetContext->master_css().clear();
    pthread_mutex_lock(&m_cssMediaMutex);
    m_cssMedia.clear();
    pthread_mutex_unlock(&m_cssMediaMutex);
    int stripped_scripts = 0;
    /* extract_scripts and the m_js* fields are engine-owned; no locking. */
    m_jsScripts.clear();
    m_jsScriptSrcs.clear();
    m_jsScriptDone.clear();
    m_jsHasInlineHandlers = false;
    m_buildHtmlContent = extract_scripts(content, m_jsEnabled ? &m_jsScripts : nullptr,
                                         m_jsEnabled ? &m_jsScriptSrcs : nullptr,
                                         m_jsEnabled ? &m_jsHasInlineHandlers : nullptr);
    /* Mark each slot ready/pending: an external <script src> starts pending and
     * is filled when its EWEB_TASK_SCRIPT fetch lands; inline bodies are ready
     * now. extract_scripts keeps m_jsScriptSrcs the same length as m_jsScripts. */
    m_jsScriptDone.assign(m_jsScripts.size(), 1);
    for(size_t i = 0; i < m_jsScriptSrcs.size() && i < m_jsScriptDone.size(); ++i) {
        if(!m_jsScriptSrcs[i].empty()) m_jsScriptDone[i] = 0;
    }
    stripped_scripts = (int)m_jsScripts.size();
    if(stripped_scripts > 0) {
        EWEB_LOG("[ewebview] extracted scripts: count=%d html_size=%d -> %d\n",
            stripped_scripts, (int)content.size(), (int)m_buildHtmlContent.size());
    }
    m_jsReparseCount = 0;
    /* document.write() output must be spliced and reparsed before anything is
     * shown, so such pages keep the legacy run-before-paint path. Every other
     * page defers its scripts to after BUILD_SWAP_DOC: the content paints
     * first and script results appear progressively (see BUILD_RUN_JS). */
    m_jsRunBeforePaint = false;
    for(size_t i = 0; i < m_jsScripts.size(); ++i) {
        if(m_jsScripts[i].find("document.write") != std::string::npos) {
            m_jsRunBeforePaint = true;
            break;
        }
    }
    m_jsNextScript = 0;
    m_jsScriptWaitSince = 0;
    m_jsPostSwapRun = false;
    m_buildHtmlUrl = m_currentHtmlUrl;
    if(!m_defaultCSSUrl.empty()) {
        m_buildPhase = BUILD_PRELOAD_CSS;
    } else {
        m_defaultCssPrepared = true;
        m_buildPhase = BUILD_CREATE_DOC;
    }

    pthread_mutex_lock(&m_taskMutex);
    m_taskQueue.clear();
    /* This document is now the initiator for everything it queues while being
     * built (CSS, images), so its subresources are same-site requests of it. */
    m_taskPageUrl = m_buildHtmlUrl;
    pthread_mutex_unlock(&m_taskMutex);

    if(!m_defaultCSSUrl.empty()) {
        bool queued = false;
        if(!m_defaultCssPrepared && !m_defaultCssLoading) {
            rememberCSS(m_defaultCSSUrl);
            m_defaultCssLoading = true;
            queued = true;
        }
        if(queued) {
            m_pendingCss++;
            EWebTask task;
            task.url = m_defaultCSSUrl;
            task.type = EWEB_TASK_CSS;
            task.loading = false;
            if(!addTask(task)) {
                m_pendingCss--;
                m_defaultCssLoading = false;
                m_defaultCssPrepared = true;
                if(m_buildPhase == BUILD_PRELOAD_CSS) {
                    m_buildPhase = BUILD_CREATE_DOC;
                }
            }
        }
    }

    /* Queue external <script src> fetches in document order. Each resolves
     * against the page URL; the result fills the matching slot(s) and re-arms
     * the RUN_JS step so runNextPageScript proceeds past it. m_taskPageUrl was
     * just set to this document, so the fetch is a same-site subresource. */
    for(size_t i = 0; i < m_jsScriptSrcs.size(); ++i) {
        if(m_jsScriptSrcs[i].empty()) continue;
        std::string abs = EWebContainer::getFullURL(&m_port, m_jsScriptSrcs[i], m_currentHtmlUrl);
        if(abs.empty()) {
            if(i < m_jsScriptDone.size()) m_jsScriptDone[i] = 1;   /* unresolvable: skip, never block */
            continue;
        }
        m_jsScriptSrcs[i] = abs;   /* results are matched by this absolute URL */
        EWebTask task;
        task.url = abs;
        task.type = EWEB_TASK_SCRIPT;
        task.loading = false;
        addTask(task);
    }

    setBuildStatus("preparing document", 5);
    EWEB_LOG("[ewebview] build queued: content_size=%d client=%dx%d\n",
        (int)content.size(), m_clientWidth, m_clientHeight);
    return true;
}

void EWebEngine::pushResult(const EWebResult& result)
{
    /* Download-worker thread. Hand the fetched/decoded payload to the engine
     * via the result queue, then wake the engine out of its park so a page
     * load never waits out an idle timeout for data that already arrived.
     * The signal is outside the result lock (no nesting: postCommand takes
     * only m_cmdMutex, the loop's park re-checks the result queue first). */
    pthread_mutex_lock(&m_resultMutex);
    m_resultQueue.push_back(result);
    if(result.type == EWEB_TASK_IMAGE) {
        EWEB_LOG("[ewebview] push image result: queue=%d url=%s hash=%08x ok=%d size=%d\n",
            (int)m_resultQueue.size(), result.url.c_str(), debug_hash_text(result.url),
            result.ok ? 1 : 0, (int)result.content.size());
    }
    pthread_mutex_unlock(&m_resultMutex);
    pthread_cond_signal(&m_cmdCond);
}

bool EWebEngine::getResult(EWebResult& result)
{
    pthread_mutex_lock(&m_resultMutex);
    if(m_resultQueue.empty()) {
        pthread_mutex_unlock(&m_resultMutex);
        return false;
    }
    result = m_resultQueue.front();
    m_resultQueue.erase(m_resultQueue.begin());
    if(result.type == EWEB_TASK_IMAGE) {
        EWEB_LOG("[ewebview] pop image result: remain=%d url=%s hash=%08x ok=%d size=%d\n",
            (int)m_resultQueue.size(), result.url.c_str(), debug_hash_text(result.url),
            result.ok ? 1 : 0, (int)result.content.size());
    }
    pthread_mutex_unlock(&m_resultMutex);
    return true;
}

bool EWebEngine::processResults()
{
    EWebResult result;
    bool drive_build = false;
    if(!getResult(result)) {
        return false;
    }

    /* A post-swap script run (m_jsPostSwapRun) is not a build for results:
     * the page on screen owns the caches, so CSS/images must keep landing in
     * it instead of stalling behind a script that may run for many ticks. */
    if(result.type == EWEB_TASK_IMAGE && m_buildPhase != BUILD_IDLE && !m_jsPostSwapRun) {
        EWEB_LOG("[ewebview] process image deferred by build: size=%d phase=%d\n",
            (int)result.content.size(), (int)m_buildPhase);
        pthread_mutex_lock(&m_resultMutex);
        m_resultQueue.insert(m_resultQueue.begin(), result);
        pthread_mutex_unlock(&m_resultMutex);
        return false;
    }
    if(result.type == EWEB_TASK_CSS &&
            m_buildPhase != BUILD_IDLE && !m_jsPostSwapRun &&
            !(m_buildPhase == BUILD_PRELOAD_CSS && result.url == m_defaultCSSUrl)) {
        pthread_mutex_lock(&m_resultMutex);
        m_resultQueue.insert(m_resultQueue.begin(), result);
        pthread_mutex_unlock(&m_resultMutex);
        return false;
    }

    uint64_t process_start = ticMs();
    EWEB_LOG("[ewebview] process result: type=%d ok=%d size=%d\n",
        result.type, result.ok ? 1 : 0, (int)result.content.size());
    if(result.type == EWEB_TASK_SCRIPT) {
        /* Fill every still-pending slot whose resolved src matches this URL
         * (addTask dedups by URL, so one fetch serves duplicate includes). A
         * failed fetch still marks the slot done with an empty body, so a
         * classic-script run never wedges behind a script that 404'd. Handled
         * before the ok/!ok split so both outcomes reach it. */
        for(size_t i = 0; i < m_jsScriptSrcs.size(); ++i) {
            if(i < m_jsScriptDone.size() && !m_jsScriptDone[i] && m_jsScriptSrcs[i] == result.url) {
                if(i < m_jsScripts.size())
                    m_jsScripts[i] = result.ok ? result.content : std::string();
                m_jsScriptDone[i] = 1;
            }
        }
        EWEB_LOG("[ewebview] script result: url=%s ok=%d size=%d\n",
            result.url.c_str(), result.ok ? 1 : 0, (int)result.content.size());
        m_deferBuildStep = true;   /* resume BUILD_RUN_JS past the filled slot */
        pthread_mutex_lock(&m_resultMutex);
        bool has_more_scripts = !m_resultQueue.empty();
        pthread_mutex_unlock(&m_resultMutex);
        return has_more_scripts;
    }
    if(!result.ok) {
        if(result.type == EWEB_TASK_IMAGE) {
            EWEB_LOG("[ewebview] image load failed\n");
        }
        if(result.type == EWEB_TASK_CSS) {
            forgetCSS(result.url);
            if(result.url == m_defaultCSSUrl) {
                m_defaultCssLoading = false;
                m_defaultCssPrepared = true;
                if(m_buildPhase == BUILD_PRELOAD_CSS) {
                    m_buildPhase = BUILD_CREATE_DOC;
                }
                drive_build = true;
            }
        }
    } else if(result.type == EWEB_TASK_HTML) {
        m_currentHtmlUrl = result.url;
        /* The visible page's URL changed: post it so the UI thread refreshes
         * the address bar / session history and fires the on_url hook there
         * (the engine never touches the embedder). */
        { EWebUiEvent uev; uev.kind = EUET_URL; uev.text = result.url; postUiEvent(uev); }
        loadHtmlContent(result.content);
        drive_build = true;
    }
    else if(result.type == EWEB_TASK_CSS) {
        loadCSSContent(result.url, result.content);
    }
    else if(result.type == EWEB_TASK_IMAGE) {
        bool mounted = false;
        if(result.image != nullptr) {
            mounted = mountDecodedImage(result.url, result.image);
            if(mounted) {
                result.image = nullptr; /* ownership moved into the cache */
            }
        } else {
            mounted = loadImageContent(result.url, (uint8_t*)result.content.data(), (int)result.content.size());
        }
        if(!mounted) {
            bool retry_later = (m_doc == nullptr && m_buildDoc == nullptr);
            if(retry_later) {
                pthread_mutex_lock(&m_resultMutex);
                m_resultQueue.insert(m_resultQueue.begin(), result);
                pthread_mutex_unlock(&m_resultMutex);
                EWEB_LOG("[ewebview] image result requeued: url=%s hash=%08x size=%d\n",
                    result.url.c_str(), debug_hash_text(result.url), (int)result.content.size());
            } else if(result.image != nullptr) {
                /* Not requeued and not mounted: release the decoded surface. */
                m_port.gfx.surface_free(m_port.gfx.ud, result.image);
                result.image = nullptr;
            }
        }
    }
    EWEB_LOG("[ewebview] process result total: type=%d cost=%u ms\n",
        result.type, (uint32_t)(ticMs() - process_start));

    if(drive_build) {
        m_deferBuildStep = true;
    }

    pthread_mutex_lock(&m_resultMutex);
    bool has_more_results = !m_resultQueue.empty();
    pthread_mutex_unlock(&m_resultMutex);
    return has_more_results;
}

void EWebEngine::markLayoutDirty(bool build)
{
    uint64_t now = ticMs();
    if(build) {
        m_buildNeedsLayout = true;
        /* Keep the original dirty-since stamp: it is what bounds the wait
         * when arrivals keep refreshing m_buildLayoutDirtyAt. */
        if(m_buildLayoutDirtySince == 0) {
            m_buildLayoutDirtySince = now;
        }
        m_buildLayoutDirtyAt = now;
    } else {
        m_needsLayout = true;
        if(m_layoutDirtySince == 0) {
            m_layoutDirtySince = now;
        }
        m_layoutDirtyAt = now;
    }
}

void EWebEngine::markContentDirty()
{
    m_contentDirty = true;
    if(m_contentDirtySince == 0) {
        m_contentDirtySince = ticMs();
    }
}

bool EWebEngine::applyPendingLayoutUpdates()
{
    /* ENGINE-THREAD ONLY. Runs the chunked master-style pass and the
     * debounced re-layout for both the build doc and the visible doc. On the
     * engine thread there is no UI-responsiveness constraint to trade
     * against: this thread exclusively owns the docs, and the wall-clock
     * budget now only bounds how long a single update_master_styles chunk
     * runs before the loop returns to drain commands and render a frame. */
    bool updated = false;
    uint64_t now = ticMs();

    /* One chunked style step per pass, shared by the build doc and the
     * visible doc (build first). A step already in flight skips the
     * debounce/pending gates so it always keeps making progress. */
    litehtml::document* style_doc = nullptr;
    bool style_build = false;
    if(m_buildDoc && m_buildNeedsStyleUpdate) {
        style_doc = m_buildDoc;
        style_build = true;
    } else if(m_doc && m_needsStyleUpdate &&
            (m_styleStepInFlight || m_pendingCss <= 0 ||
             (m_styleNeedSince != 0 && (now - m_styleNeedSince) > kStyleMaxWaitMs) ||
             (m_layoutDirtyAt != 0 && (now - m_layoutDirtyAt) > 3000))) {
        style_doc = m_doc;
    }
    bool style_step_running = m_styleStepInFlight ||
            (style_doc != nullptr && style_doc->style_step_active());
    if(style_doc != nullptr && !style_step_running &&
            !style_build && m_layoutDirtyAt != 0 &&
            (now - m_layoutDirtyAt) < kLayoutDebounceMs) {
        style_doc = nullptr; /* fresh start stays behind the debounce gate */
    }
    if(style_doc != nullptr && !style_build &&
            (m_styleStepInFlight || style_doc->style_step_active()) &&
            m_layoutDirtyAt != 0 && (now - m_layoutDirtyAt) > 3000) {
        /* 3s force point hit while a step was in flight: the stylesheet set
         * may have grown since it started, so restart the walk from scratch.
         * The next tick begins a fresh phase-0 chunk. */
        style_doc->abort_style_step();
        m_styleStepInFlight = false;
        style_doc = nullptr;
    }
    if(style_doc != nullptr) {
        uint64_t chunk_start = ticMs();
        bool done = style_doc->update_master_styles_step(chunk_start + kStyleBudgetIdleMs);
        uint32_t chunk_ms = (uint32_t)(ticMs() - chunk_start);
        m_styleStepInFlight = !done;
        if(done) {
            if(style_build) {
                EWEB_LOG("[ewebview] apply build styles: %u ms (chunked)\n", chunk_ms);
                m_buildNeedsStyleUpdate = false;
            } else {
                EWEB_LOG("[ewebview] apply css-update: chunk=%u ms (chunked)\n", chunk_ms);
                m_needsStyleUpdate = false;
                m_styleNeedSince = 0;
                /* Layout now reflects the final styles; ask for one render. */
                markLayoutDirty(false);
            }
            updated = true;
        } else {
            EWEB_LOG("[ewebview] style step chunk: %u ms phase=%d stamped=%u visits=%u\n",
                chunk_ms, style_doc->style_step_phase(),
                style_doc->style_step_stamped(), style_doc->style_step_visits());
        }
    }

    /* The style chunk above can take its whole budget, so re-read the clock:
     * the caps below are wall-clock promises to the user. */
    now = ticMs();

    if(m_buildDoc) {
        bool build_quiet = (m_buildLayoutDirtyAt == 0 ||
                (now - m_buildLayoutDirtyAt) >= kLayoutDebounceMs);
        bool build_forced = (m_buildLayoutDirtySince != 0 &&
                (now - m_buildLayoutDirtySince) >= kLayoutMaxWaitMs);
        if(!style_build && (m_buildNeedsStyleUpdate || m_buildNeedsLayout) &&
                !build_quiet && !build_forced) {
            return updated;
        }
        /* While the build doc's style step is in flight
         * m_buildNeedsStyleUpdate stays set, so the render below waits for
         * the walk to complete. */
        if(!m_buildAbort && !m_buildNeedsStyleUpdate && m_buildNeedsLayout) {
            uint64_t render_start = ticMs();
            m_buildDoc->render(m_clientWidth);
            uint32_t render_ms = (uint32_t)(ticMs() - render_start);
            EWEB_LOG("[ewebview] render(build-pending): %u ms forced=%d\n",
                render_ms, build_forced ? 1 : 0);
            m_buildNeedsLayout = false;
            m_buildLayoutDirtyAt = 0;
            m_buildLayoutDirtySince = 0;
            updated = true;
        }
    } else {
        m_buildNeedsStyleUpdate = false;
        m_buildNeedsLayout = false;
        m_buildLayoutDirtyAt = 0;
        m_buildLayoutDirtySince = 0;
    }

    if(m_doc) {
        /* Render only once this doc's chunked style update has completed;
         * laying out half-styled content would just be thrown away. A step
         * running for the build doc does not block the visible doc. */
        bool style_pending = m_needsStyleUpdate ||
                (m_styleStepInFlight && style_doc == m_doc);
        bool quiet = (m_layoutDirtyAt == 0 ||
                (now - m_layoutDirtyAt) >= kLayoutDebounceMs);
        bool forced = (m_layoutDirtySince != 0 &&
                (now - m_layoutDirtySince) >= kLayoutMaxWaitMs);
        /* Images keep landing while stylesheets are still in flight, and
         * m_pendingCss > 0 postpones the style walk itself. Without this cap
         * a freshly displayed page holds every image at its pre-arrival 0x0
         * box until the last sheet lands, so past the cap lay out
         * progressively; the style walk marks layout dirty again when it
         * finishes and the page settles on the fully styled layout. */
        bool behind_style = style_pending && m_layoutDirtySince != 0 &&
                (now - m_layoutDirtySince) >= kLayoutBehindStyleMs;
        /* A pending build abort skips the re-layout: the engine loop is about
         * to tear the build down and swap or keep the visible doc, so a full
         * render() pass here would be thrown away. buildAbortRequested() is
         * phase-gated, so the stale flag left by a STOP on an idle page does
         * not freeze the visible page - its layout, still marked dirty, runs
         * on the next pass once the flag clears in cleanupBuildResources. */
        if(!buildAbortRequested() && m_needsLayout && (quiet || forced) && (!style_pending || behind_style)) {
            uint64_t render_start = ticMs();
            m_doc->render(m_clientWidth);
            uint32_t layout_ms = (uint32_t)(ticMs() - render_start);
            EWEB_LOG("[ewebview] render(pending): %u ms forced=%d behind_style=%d\n",
                layout_ms, forced ? 1 : 0, behind_style ? 1 : 0);
            m_needsLayout = false;
            m_layoutDirtyAt = 0;
            m_layoutDirtySince = 0;
            markContentDirty();
            updated = true;
        }
    } else {
        m_needsStyleUpdate = false;
        m_styleNeedSince = 0;
        m_needsLayout = false;
        m_layoutDirtyAt = 0;
        m_layoutDirtySince = 0;
        m_styleStepInFlight = false;
    }

    return updated;
}

void EWebEngine::advanceBuildStep()
{
    if(m_buildPhase == BUILD_IDLE) {
        return;
    }

    /* A termination request (stop, address-bar load, reload, link click)
     * raised m_buildAbort - possibly from the UI thread while this build was
     * mid-parse. Throw the half-built page away in the engine's own context.
     * The pending navigation is the page the user just asked for, so it must
     * survive cleanupBuildResources(), which clears it as part of the
     * teardown. */
    if(m_buildAbort) {
        std::string pending_nav = m_jsPendingNav;
        cleanupBuildResources();
        m_jsPendingNav = pending_nav;
        EWEB_LOG("[ewebview] build aborted\n");
        return;
    }

    if(m_buildPhase == BUILD_PRELOAD_CSS) {
        setBuildStatus("loading styles", 15);
        bool ready = m_defaultCssPrepared || m_defaultCSSUrl.empty();
        bool waiting = m_defaultCssLoading;
        if(ready || !waiting) {
            m_buildPhase = BUILD_CREATE_DOC;
        }
        return;
    }

    if(m_buildPhase == BUILD_CREATE_DOC) {
        setBuildStatus("building document", 45);
        if(!m_buildTargetContext) {
            m_buildTargetContext = (m_activeContext == &m_browser_context) ? &m_buildContext : &m_browser_context;
            m_buildTargetContext->master_css().clear();
        }
        if(m_buildContainer) {
            delete m_buildContainer;
            m_buildContainer = nullptr;
        }
        if(m_buildDoc) {
            delete m_buildDoc;
            m_buildDoc = nullptr;
        }
        litehtml::context* build_ctx = m_buildTargetContext ? m_buildTargetContext : &m_buildContext;
        build_ctx->set_fast_mode(true);
        m_buildContainer = new EWebContainer(&m_port, this);
        m_buildContainer->set_client_size(m_clientWidth, m_clientHeight);
        /* Page URL drives relative/root-relative resolution for stylesheets
         * and images discovered while the DOM is being created. */
        m_buildContainer->set_base_url(m_buildHtmlUrl.c_str());
        m_buildContainer->setDeferImageLoad(true);
        m_buildContainer->resetPerfStats();
        if(m_buildHtmlContent.empty()) {
            EWEB_LOG("[ewebview] BUILD_CREATE_DOC: empty content, aborting build\n");
            m_buildPhase = BUILD_FAILED;
            setBuildStatus("no content to render", 100);
            return;
        }
        uint64_t create_start = ticMs();
        /* The parse is the longest single litehtml call (whole-DOM build). It
         * runs on the engine thread, so it can never freeze the embedder -
         * but it must still be interruptible: a STOP/NAVIGATE/RELOAD raised
         * m_buildAbort from the UI thread, and the container's
         * create_element/text_width callbacks poll it (buildAbortRequested)
         * so the parse unwinds fast. */
        m_buildDoc = litehtml::document::createFromString(m_buildHtmlContent.c_str(), m_buildContainer, build_ctx);
        uint32_t create_ms = (uint32_t)(ticMs() - create_start);
        if(m_buildAbort) {
            /* A termination request landed mid-parse; the half-built doc is
             * discarded at the top of the next advanceBuildStep. */
            return;
        }
        if(!m_buildDoc) {
            m_buildPhase = BUILD_FAILED;
            setBuildStatus("document build failed", 100);
            return;
        }
        EWEB_LOG("[ewebview] create dom: %u ms\n", create_ms);
        /* Restore script-applied DOM mutations lost when a document.write()
         * reparse rebuilt the doc from source; a no-op on the first pass. */
        replayJsMutations();
        /* Only document.write() pages still run their scripts before the
         * first paint; everything else paints first and runs its scripts in
         * the post-swap BUILD_RUN_JS phase so results show up
         * progressively. */
        m_buildPhase = (m_jsRunBeforePaint && !m_jsScripts.empty())
                     ? BUILD_RUN_JS : BUILD_RENDER_DOC;
        return;
    }

    if(m_buildPhase == BUILD_RUN_JS && !m_jsPostSwapRun) {
        /* Pre-render mode (document.write pages): run every inline script
         * against the freshly built DOM, then splice any document.write()
         * output back into the pending HTML and restart the build so the
         * written markup is parsed. Runs on the engine thread, which owns the
         * VM and m_buildDoc; the DOM-bridge callbacks touch it
         * synchronously. */
        setBuildStatus("running scripts", 60);
        if(m_buildDoc == nullptr) {
            m_buildPhase = BUILD_RENDER_DOC;
            return;
        }
        bool scripts_done = runPageScripts();
        if(m_buildAbort) {
            /* A termination request landed during the script run: skip the
             * write-splice, the page dies at the top of the next step. */
            return;
        }
        if(!scripts_done) {
            /* Parked on an in-flight <script src> fetch (document order): stay
             * in this phase and re-enter on the next tick or the instant
             * processResults re-arms the step via m_deferBuildStep. */
            m_buildPhase = BUILD_RUN_JS;
            return;
        }
        bool restart = applyJsWriteBuffer();
        /* applyJsWriteBuffer() already tore down buildDoc/buildContainer and
         * updated m_buildHtmlContent when it returns true; rebuild from
         * it. */
        m_buildPhase = restart ? BUILD_CREATE_DOC : BUILD_RENDER_DOC;
        return;
    }

    if(m_buildPhase == BUILD_RUN_JS) {
        /* Post-swap mode: the page is already on screen. Run one script per
         * engine step against the visible document; bridge mutations repaint
         * mid-script through jsProgressiveFlush(), so a test page shows its
         * results as they are produced instead of freezing on "loading". */
        setBuildStatus("running scripts", 100);
        bool more = runNextPageScript();
        if(m_buildAbort) {
            /* Stop/reload requested mid-script: run no further scripts and
             * fire no load events on a page the user just cancelled; the
             * teardown runs at the top of the next step. */
            return;
        }
        bool restart = false;
        if(!more) {
            /* A deferred script still reached document.write() (the substring
             * scan missed an indirect call): splice and rebuild as usual. */
            restart = applyJsWriteBuffer();
        }
        if(!more && !restart) {
            m_buildHtmlContent.clear();
            m_buildHtmlUrl.clear();
            m_jsPostSwapRun = false;
            m_buildPhase = BUILD_IDLE;
        }
        if(more) {
            return;
        }
        if(restart) {
            m_jsPostSwapRun = false;
            m_buildPhase = BUILD_CREATE_DOC;
            return;
        }
        setBuildStatus("", 0);
        postScrollClamp();
        /* DOMContentLoaded / load fire only now, after the page's inline
         * scripts have finished - the order every page assumes. */
        jsFireLoadEvents();
        markContentDirty();
        return;
    }

    if(m_buildPhase == BUILD_RENDER_DOC) {
        setBuildStatus("layout and first paint", 80);
        if(m_buildDoc) {
            /* The DOM was created in fast mode, whose parse_styles path skips
             * the whole box model (height/width stay predef(0)), so layout
             * would collapse 100vh bodies and attribute-sized canvases to 0.
             * Clear fast mode and force one full style pass BEFORE the
             * initial render; render() only lays out, it never re-parses
             * styles. */
            if(m_buildTargetContext) {
                m_buildTargetContext->set_fast_mode(false);
            }
            /* Chunked style walk instead of one monolithic
             * update_master_styles(): each chunk is bounded by
             * kStyleBudgetIdleMs so the loop returns to drain commands often,
             * and m_buildAbort - raised by the UI thread the instant a
             * STOP/NAVIGATE/RELOAD is queued - breaks it mid-walk. */
            m_buildDoc->abort_style_step();
            bool styles_done = false;
            while(!styles_done && !m_buildAbort) {
                styles_done = m_buildDoc->update_master_styles_step(
                        ticMs() + kStyleBudgetIdleMs);
            }
            uint64_t render_start = ticMs();
            if(!m_buildAbort)
                m_buildDoc->render(m_clientWidth);
            uint32_t render_ms = (uint32_t)(ticMs() - render_start);
            uint32_t text_width_calls = 0, text_width_ms = 0, draw_text_calls = 0, draw_text_ms = 0;
            uint32_t text_width_hits = 0, text_width_misses = 0;
            uint32_t char_width_hits = 0, char_width_misses = 0;
            uint32_t create_font_calls = 0, create_font_ms = 0;
            m_buildContainer->getPerfStats(text_width_calls, text_width_ms, draw_text_calls, draw_text_ms,
                                           text_width_hits, text_width_misses,
                                           char_width_hits, char_width_misses,
                                           create_font_calls, create_font_ms);
            EWEB_LOG("[ewebview] doc ready: width=%d height=%d\n", m_buildDoc->width(), m_buildDoc->height());
            EWEB_LOG("[ewebview] render(initial): %u ms\n", render_ms);
            EWEB_LOG("[ewebview] build perf: text_width=%u/%u ms hit=%u miss=%u char_hit=%u char_miss=%u create_font=%u/%u ms\n",
                text_width_calls, text_width_ms, text_width_hits, text_width_misses,
                char_width_hits, char_width_misses,
                create_font_calls, create_font_ms);
        }
        if(m_buildAbort) {
            return;   /* consumed at the top of the next step */
        }
        m_buildPhase = BUILD_SWAP_DOC;
        return;
    }

    if(m_buildPhase == BUILD_SWAP_DOC) {
        setBuildStatus("displaying page", 100);
        // Save old pointers (engine-owned; freed on a later loop iteration)
        EWebContainer* old_container = m_container;
        litehtml::document::ptr old_doc = m_doc;

        // Install the new page.
        m_doc = m_buildDoc;
        m_container = m_buildContainer;
        m_buildDoc = nullptr;
        m_buildContainer = nullptr;

        if(m_buildTargetContext) {
            m_activeContext = m_buildTargetContext;
            m_activeContext->set_fast_mode(false);
        }
        if(m_container) {
            m_container->setDeferImageLoad(false);
        }
        /* Scripts that did not run before paint execute next, against the
         * page just put on screen. Keep the page source and URL around for
         * them: document.write() (applyJsWriteBuffer) splices into the source
         * and location.href reads the URL; both are cleared when the run
         * ends. */
        bool run_after = m_jsEnabled && !m_jsRunBeforePaint &&
                (m_jsNextScript < m_jsScripts.size() ||
                 (m_jsVm == nullptr && m_jsHasInlineHandlers));
        if(!run_after) {
            m_buildHtmlContent.clear();
            m_buildHtmlUrl.clear();
        }
        m_jsPostSwapRun = run_after;
        m_buildPhase = run_after ? BUILD_RUN_JS : BUILD_IDLE;
        m_defaultCssPrepared = false;
        m_defaultCssLoading = false;
        m_buildTargetContext = nullptr;
        m_flushDeferredImages = true;
        /* A brand-new page starts at the top. m_engineScroll* is the engine's
         * authoritative offset; the UI mirror is reset by the SCROLL_CLAMP
         * posted below. The cache still holds the old page, so invalidate
         * it - the engine loop re-renders the new one and posts a fresh
         * frame. */
        m_engineScrollX = 0;
        m_engineScrollY = 0;
        m_cacheValid = false;

        // Defer deletion of the old page to a later loop iteration to prevent
        // re-entrant corruption (the engine loop flushes these at its top).
        if(m_pendingDeleteDoc) delete m_pendingDeleteDoc;
        if(m_pendingDeleteContainer) delete m_pendingDeleteContainer;
        m_pendingDeleteDoc = old_doc;
        m_pendingDeleteContainer = old_container;

        postScrollClamp();
        if(run_after) {
            /* Page is visible; the app status line shows the script run while
             * the overlay stays off (see the m_jsPostSwapRun gate in
             * setBuildStatus). Load events fire when the scripts finish. */
            setBuildStatus("running scripts", 100);
        } else {
            setBuildStatus("", 0);
            /* DOMContentLoaded, then window/document load (and <body onload>,
             * which the event bridge treats as the window's load slot). Fired
             * here rather than in BUILD_RUN_JS so handlers see the laid-out,
             * on-screen document - the same element pointers they will keep
             * using afterwards, since the swap above hands over the very same
             * document object. */
            jsFireLoadEvents();
        }
        markContentDirty();
        return;
    }

    if(m_buildPhase == BUILD_FAILED) {
        cleanupBuildResources();
    }
}

void EWebEngine::drawPageToCacheLocked(int stripY, int stripH)
{
    /* ENGINE-THREAD ONLY: m_doc and m_pageCache (the pooled buffer the engine
     * is drawing into) are valid. The cache is viewport-local: the document
     * origin is (-m_engineScrollX, -m_engineScrollY) and the clip strip is in
     * cache coordinates. The surface clip backs up litehtml's own per-element
     * clipping (does_intersect) so a partial strip draw can never spill over
     * the shifted rows around it. */
    eweb_surface_t* cache = m_pageCache;
    int cacheW = surfaceW(cache);
    int cacheH = surfaceH(cache);
    if(stripY < 0) stripY = 0;
    if(stripY + stripH > cacheH) stripH = cacheH - stripY;
    if(stripH <= 0)
        return;

    if(m_port.gfx.surface_set_clip != nullptr)
        m_port.gfx.surface_set_clip(m_port.gfx.ud, cache, 0, stripY, cacheW, stripH);
    if(m_port.gfx.fill_rect != nullptr)
        m_port.gfx.fill_rect(m_port.gfx.ud, cache, 0, stripY, cacheW, stripH, 0xFFFFFFFFu);
    if(m_container) {
        m_container->resetPerfStats();
    }
    uint64_t draw_start = ticMs();
    litehtml::position pos(0, stripY, cacheW, stripH);
    m_doc->draw((litehtml::uint_ptr)cache, -m_engineScrollX, -m_engineScrollY, &pos);
    /* Blit any <canvas> backing stores over the laid-out page, using the same
     * offset as m_doc->draw so canvases track scroll; the clip confines them
     * to the strip. litehtml itself draws nothing for <canvas>. */
    compositeCanvases(cache, -m_engineScrollX, -m_engineScrollY);
    /* An open <select> dropdown floats above the page: draw it last so it is
     * never covered by the laid-out document. No-op unless one is open. */
    drawSelectPopup(cache);
    if(m_port.gfx.surface_unset_clip != nullptr)
        m_port.gfx.surface_unset_clip(m_port.gfx.ud, cache);
    uint32_t draw_ms = (uint32_t)(ticMs() - draw_start);
    if(m_container != NULL && draw_ms >= 20) {
        uint32_t text_width_calls = 0, text_width_ms = 0, draw_text_calls = 0, draw_text_ms = 0;
        uint32_t text_width_hits = 0, text_width_misses = 0;
        uint32_t char_width_hits = 0, char_width_misses = 0;
        uint32_t create_font_calls = 0, create_font_ms = 0;
        m_container->getPerfStats(text_width_calls, text_width_ms, draw_text_calls, draw_text_ms,
                                  text_width_hits, text_width_misses,
                                  char_width_hits, char_width_misses,
                                  create_font_calls, create_font_ms);
        EWEB_LOG("[ewebview] draw(cache strip=%d+%d): %u ms text_width=%u/%u ms hit=%u miss=%u char_hit=%u char_miss=%u draw_text=%u/%u ms create_font=%u/%u ms\n",
            stripY, stripH, draw_ms, text_width_calls, text_width_ms, text_width_hits, text_width_misses,
            char_width_hits, char_width_misses,
            draw_text_calls, draw_text_ms, create_font_calls, create_font_ms);
    }
}

void EWebEngine::handleAnchorClick(int cx, int cy)
{
    /* ENGINE-THREAD ONLY: follow an <a href> under a left-button release. The
     * UI thread forwarded the viewport-relative client coords (cx, cy);
     * hit-testing the document and starting the navigation both happen here,
     * where the document lives. */

    /* A drag-scroll that happens to release over a link must not navigate;
     * only treat the gesture as a click when the pointer barely moved since
     * the press (m_pressX/Y were captured on the matching ECMD_INPUT down).
     * Consumed either way so the next gesture starts clean. */
    const int slop = 8;
    bool moved = m_pressValid &&
        (cx - m_pressX > slop || cx - m_pressX < -slop ||
         cy - m_pressY > slop || cy - m_pressY < -slop);
    m_pressValid = false;
    if(moved)
        return;

    /* The page on screen (m_doc/m_container), not jsActiveDoc(): a build in
     * flight renders nothing new yet, so the user is clicking the old
     * page. */
    litehtml::document* doc = m_doc;
    EWebContainer* container = m_container;
    if(doc == nullptr || container == nullptr)
        return;

    litehtml::element::ptr root = doc->root();
    if(root == nullptr)
        return;

    /* Hit test in unscrolled document coordinates (add the engine scroll
     * offsets); the client-relative pair is what position:fixed elements test
     * against. */
    litehtml::element::ptr target =
        root->get_element_by_point(cx + m_engineScrollX, cy + m_engineScrollY, cx, cy);

    /* Walk up to the nearest <a href>, mirroring el_anchor::on_click()'s
     * parent recursion (html_tag::on_click bubbles to the parent). The hit
     * element is usually the text node inside the anchor, so the walk is what
     * finds it. */
    for(litehtml::element::ptr el = target; el != nullptr; el = el->parent()) {
        const litehtml::tchar_t* tag = el->get_tagName();
        if(tag == nullptr || t_strcasecmp(tag, _t("a")) != 0)
            continue;
        const litehtml::tchar_t* href = el->get_attr(_t("href"));
        if(href != nullptr && href[0] != 0) {
            /* on_anchor_click -> queueNavigation records m_jsPendingNav and
             * raises the abort; the engine loop performs the navigation once
             * this dispatch unwinds (jsRunPendingNavigation ->
             * engineNavigate). */
            container->on_anchor_click(href, el);
            break;
        }
    }
}

} /* namespace eweb */

/* ==================================================================
 * Public C API (ewebview.h)
 * ================================================================== */

struct ewebview {
    eweb::EWebEngine engine;
    explicit ewebview(const eweb_port_t* port) : engine(port) {}
};

extern "C" {

void eweb_port_init(eweb_port_t* port)
{
    /* Platform-independent (a plain memset), so it lives in the core and not
     * in any port. */
    if(port != NULL)
        memset(port, 0, sizeof(*port));
}

void eweb_listener_init(eweb_listener_t* l)
{
    if(l != NULL)
        memset(l, 0, sizeof(*l));
}

ewebview_t* ewebview_create(const eweb_port_t* port)
{
    if(port == NULL)
        return NULL;
    /* REQUIRED tables (see ewebview_port.h): surfaces, fonts, the clock, and
     * the two blits used for background images / canvas compositing. */
    if(port->gfx.surface_new == NULL || port->gfx.surface_free == NULL ||
       port->gfx.surface_dims == NULL ||
       port->gfx.blit == NULL || port->gfx.blit_fit_alpha == NULL)
        return NULL;
    if(port->font.create == NULL || port->font.destroy == NULL ||
       port->font.metrics == NULL || port->font.char_width == NULL ||
       port->font.draw_text == NULL)
        return NULL;
    if(port->clock.tic_ms == NULL)
        return NULL;
    return new ewebview(port);
}

void ewebview_destroy(ewebview_t* v)
{
    delete v;
}

void ewebview_set_listener(ewebview_t* v, const eweb_listener_t* listener)
{
    if(v != NULL)
        v->engine.setListener(listener);
}

void ewebview_set_viewport(ewebview_t* v, int width, int height)
{
    if(v != NULL)
        v->engine.setViewport(width, height);
}

void ewebview_set_default_css(ewebview_t* v, const char* url)
{
    if(v != NULL)
        v->engine.setDefaultCSS(url);
}

void ewebview_set_js_enabled(ewebview_t* v, bool enabled)
{
    if(v != NULL)
        v->engine.setJSEnabled(enabled);
}

void ewebview_load(ewebview_t* v, const char* url)
{
    if(v != NULL)
        v->engine.load(url);
}

void ewebview_stop(ewebview_t* v)
{
    if(v != NULL)
        v->engine.stop();
}

void ewebview_reload(ewebview_t* v)
{
    if(v != NULL)
        v->engine.reload();
}

const char* ewebview_get_url(ewebview_t* v)
{
    return (v != NULL) ? v->engine.currentUrlUi() : "";
}

void ewebview_post_event(ewebview_t* v, const eweb_event_t* ev)
{
    if(v != NULL && ev != NULL)
        v->engine.postInput(*ev);
}

void ewebview_post_key(ewebview_t* v, const eweb_key_event_t* ev)
{
    if(v != NULL && ev != NULL)
        v->engine.postKey(*ev);
}

void ewebview_scroll(ewebview_t* v, int x, int y)
{
    if(v != NULL)
        v->engine.scrollTo(x, y);
}

void ewebview_tick(ewebview_t* v)
{
    if(v != NULL)
        v->engine.tick();
}

void ewebview_release_frame(ewebview_t* v, eweb_surface_t* frame)
{
    if(v != NULL)
        v->engine.releaseFrame(frame);
}

} /* extern "C" */
