/* EWebJs.cc - the mario JS VM lifecycle and the DOM/event/web bridges.
 *
 * Merges what the widget++ original split across two files:
 *   - WidgetWebview.cc's JS section: the mario platform hooks, the VM
 *     lifecycle (init/reset, page-script runners, the document.write reparse
 *     path, the run-budget watchdog) and the core DOM bridge callbacks.
 *   - WidgetWebviewJs.cc: the extended DOM bridge callbacks (querySelector,
 *     tree walking/mutation, geometry, computed style) plus the Event and
 *     BOM (js_event.h / js_web.h) embedder glue.
 *
 * Threading: every callback runs synchronously on the ENGINE thread from
 * inside vm_load_run() / js_dom_poll_timers() / js_event_dispatch(). The
 * engine exclusively owns the VM and the documents, so no locking is needed -
 * but a callback cannot tear down the page it is running in, so navigation
 * and script-driven scrolling are deferred through m_jsPendingNav /
 * m_jsScrollPending and run by the engine loop once the script unwinds.
 *
 * Element handles are litehtml::element* into whichever document jsActiveDoc()
 * currently returns; BUILD_SWAP_DOC hands the same document over, so they stay
 * valid for the whole life of a page. char* returns are allocated with
 * mario_malloc() and freed by the natives via mario_free().
 *
 * Nothing here names EwokOS: the clock comes from the port's clock table,
 * pointer plausibility from the (optional) sys.ptr_sane hook, and JS console
 * output goes to the (optional) sys.log hook.
 */

#include "EWebInternal.h"
#include "EWebLog.h"
#include "EWebCookies.h"
#include <algorithm>   /* std::find (runaway/requeue bookkeeping) */
#include "eweb_el_input.h"

#include <mario/mario.h>
#include <mario/js_dom.h>
#include <mario/js_event.h>
#include <mario/js_web.h>

/* el_text is not pulled in by <litehtml.h>; createTextNode and the text
 * setters build one directly (malloc + placement-new, matching litehtml_alloc). */
#include <litehtml/el_text.h>

#include <string>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <new>

/* mario entry points that live in libmario.a but are not declared in a public
 * header: the JS bytecode compiler (passed to vm_new) and the all-natives
 * registrar (passed to vm_init so console/Object/Array/... exist). */
extern "C" bool js_compile(bytecode_t* bc, const char* input);
extern "C" void reg_all_natives(vm_t* vm);

namespace eweb {

/* Diagnostic dump: with EWEB_DUMP_SCRIPTS=/dir set, every page script is
 * written to <dir>/script_<i>.js (failures also get logged) so a compile or
 * runtime error can be replayed offline through the host mario CLI. */
static void jsDumpScript(size_t i, const std::string& src, bool failed)
{
    static const char* dir = getenv("EWEB_DUMP_SCRIPTS");
    if(dir == nullptr || dir[0] == 0) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/script_%02d%s.js", dir, (int)i, failed ? "_FAIL" : "");
    FILE* f = fopen(path, "wb");
    if(f == nullptr) return;
    fwrite(src.data(), 1, src.size(), f);
    fclose(f);
}

/* ==================================================================
 * mario platform hooks
 *
 * mario allocates everything through three global function pointers that are
 * NULL until the embedder sets them (vm_new refuses to run otherwise). The
 * malloc/free pair maps straight onto libc; _platform_out carries JS console
 * text (console.log, the alert() fallback, ...) to the port's sys.log hook.
 *
 * _platform_out is a process-wide C global with no user-data slot, so the
 * target log hook is kept in a file-static pair refreshed by every
 * initJsVm() - the engine whose VM ran last receives the output, which is the
 * right scoping for the single-browser use case (and with no hook installed
 * the output is simply dropped).
 * ================================================================== */

static void  (*s_js_log_fn)(void* ud, const char* text) = nullptr;
static void*   s_js_log_ud = nullptr;

static void* js_platform_malloc(uint32_t size) { return malloc((size_t)size); }
static void  js_platform_free(void* p)         { free(p); }

static void  js_platform_out(const char* s)
{
    if(s == nullptr || s_js_log_fn == nullptr) return;
    std::string line("[js] ");
    line += s;
    s_js_log_fn(s_js_log_ud, line.c_str());
}

/* ==================================================================
 * Local helpers
 * ================================================================== */

/* Copy a C string into a mario_malloc'd buffer the natives can adopt. */
static char* js_strdup_mario(const char* s)
{
    if(s == nullptr) return nullptr;
    size_t n = strlen(s);
    char* p = (char*)mario_malloc((uint32_t)n + 1);
    if(p == nullptr) return nullptr;
    if(n > 0) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* Drop a parked node from the detached list once a script re-inserts it, so
 * jsFreeDetachedNodes() cannot delete something that is back in the tree. */
static void js_unpark(std::vector<litehtml::element*>& parked, litehtml::element* el)
{
    for(size_t i = 0; i < parked.size(); ++i) {
        if(parked[i] == el) {
            parked.erase(parked.begin() + i);
            return;
        }
    }
}

/* Best-effort tag stripper for the innerHTML setter: litehtml exposes no HTML
 * fragment parser, so markup assigned to innerHTML is reduced to its text. */
static std::string js_strip_tags(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    bool in_tag = false;
    for(size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if(c == '<') { in_tag = true; continue; }
        if(c == '>') { in_tag = false; continue; }
        if(!in_tag) out += c;
    }
    return out;
}

/* Replace an element's children with a single text node carrying `text`.
 * The removed children are PARKED into `park` (the caller's m_jsDetached)
 * rather than deleted: a script can still hold an element handle or listener
 * keyed on any of them, and freeing here would dangle it - the same invariant
 * jsElRemoveChild upholds. Parking keeps the whole detached subtree alive
 * until jsFreeDetachedNodes() runs at page teardown. When `park` is null (no
 * owning engine, e.g. a build-time replay with nowhere to park) the old
 * delete-in-place behaviour is kept. The new el_text is built like
 * litehtml_alloc, appended (which sets its parent), then measured via
 * parse_styles so it renders. ENGINE-THREAD ONLY (the exclusive doc owner). */
static void js_set_element_text(litehtml::element* e, const char* text,
                                std::vector<litehtml::element*>* park)
{
    if(e == nullptr) return;
    litehtml::document* doc = e->get_document();
    while(e->get_children_count() > 0) {
        litehtml::element::ptr c = e->get_child(0);
        if(c == nullptr) break;
        e->removeChild(c);          /* unlink first so ~document won't re-free */
        if(park != nullptr) {
            park->push_back(c);     /* keep alive: JS may still reference it */
        } else {
            delete c;
        }
    }
    if(doc == nullptr || text == nullptr || text[0] == 0) return;
    void* mem = malloc(sizeof(litehtml::el_text));
    if(mem == nullptr) return;
    litehtml::el_text* t = new (mem) litehtml::el_text(text, doc);
    if(e->appendChild(t)) {
        t->parse_styles(false);
    } else {
        delete t;
    }
}

/* Apply an innerHTML fragment to `e`: drop its current children (parking them
 * like js_set_element_text), parse `html` through the document's fragment
 * parser, then append and style each resulting node. create_fragment runs the
 * same create_node path a full parse uses, so tags become the right element
 * subclasses (el_image for <img>, ...). style_detached_subtree matches the
 * cascade and runs parse_attributes (so <img src> resolves); parse_styles then
 * recurses, so a nested <img> reaches el_image::parse_styles -> load_image and
 * its fetch is queued even while `e` is itself detached (same container/engine).
 * ENGINE-THREAD ONLY (the exclusive doc owner). */
static void js_apply_inner_html(litehtml::element* e, const std::string& html,
                                std::vector<litehtml::element*>* park)
{
    if(e == nullptr) return;
    litehtml::document* doc = e->get_document();
    while(e->get_children_count() > 0) {
        litehtml::element::ptr c = e->get_child(0);
        if(c == nullptr) break;
        e->removeChild(c);
        if(park != nullptr) park->push_back(c);
        else delete c;
    }
    if(doc == nullptr || html.empty()) return;
    litehtml::elements_vector nodes;
    doc->create_fragment(html.c_str(), nodes);
    EWEB_LOG("[ewebview] inner_html: created %d fragment nodes for '%.40s'\n",
             (int)nodes.size(), html.c_str());
    for(size_t i = 0; i < nodes.size(); ++i) {
        litehtml::element* n = nodes[i];
        if(n == nullptr) continue;
        if(!e->appendChild(n)) {
            if(park != nullptr) park->push_back(n);
            else delete n;
            continue;
        }
        EWEB_LOG("[ewebview] inner_html: node %d appended, styling\n", (int)i);
        doc->style_detached_subtree(n);
        n->parse_styles(false);
        EWEB_LOG("[ewebview] inner_html: node %d styled ok\n", (int)i);
    }
    EWEB_LOG("[ewebview] inner_html: done\n");
}

/* ==================================================================
 * VM lifecycle
 * ================================================================== */

void EWebEngine::initJsVm()
{
    if(m_jsVm != nullptr) return;

    /* mario routes every allocation through these globals; install them once
     * (process-wide) before vm_new(). See mario_js/bin/mario/mario.c. */
    if(_platform_malloc == nullptr) _platform_malloc = js_platform_malloc;
    if(_platform_free   == nullptr) _platform_free   = js_platform_free;
    if(_platform_out    == nullptr) _platform_out    = js_platform_out;
    /* Point the console-output trampoline at THIS engine's log hook. */
    s_js_log_fn = m_port.sys.log;
    s_js_log_ud = m_port.sys.ud;

    m_jsVm = vm_new(js_compile, VAR_CACHE_MAX_DEF, LOAD_NCACHE_MAX_DEF);
    if(m_jsVm == nullptr) {
        EWEB_LOG("[ewebview] js: vm_new failed\n");
        return;
    }
    /* reg_all_natives installs the JS builtins (Object/Array/String/console). */
    vm_init(m_jsVm, reg_all_natives, nullptr);

    /* Instruction-level service cadence (see mario.h): the step hook enforces
     * the run budget and honours a termination abort (m_buildAbort) so a long or
     * hostile script unwinds fast. Costs nothing while unset. */
    m_jsVm->step_interval = 32768;
    m_jsVm->on_step       = jsVmStepHook;
    m_jsVm->on_step_data  = this;

    js_dom_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.alert             = jsAlert;
    cb.document_write    = jsDocumentWrite;
    cb.get_title         = jsGetTitle;
    cb.set_title         = jsSetTitle;
    cb.get_url           = jsGetUrl;
    cb.get_element_by_id = jsGetElementById;
    cb.el_get_text       = jsElGetText;
    cb.el_set_text       = jsElSetText;
    cb.el_get_html       = jsElGetHtml;
    cb.el_set_html       = jsElSetHtml;
    cb.el_get_attr       = jsElGetAttr;
    cb.el_set_attr       = jsElSetAttr;
    cb.el_get_tag        = jsElGetTag;
    /* Element.id needs no dedicated callback: the DOM bridge reads it through
     * el_get_attr(ctx, el, "id"), which is wired up just above. */
    /* Document structure, selector queries and node construction. They resolve
     * against jsActiveDoc(), which is the build document while scripts run and
     * the visible one afterwards. */
    cb.get_root          = jsGetRoot;
    cb.get_body          = jsGetBody;
    cb.get_head          = jsGetHead;
    cb.query_all         = jsQueryAll;
    cb.create_element    = jsCreateElement;
    cb.create_text_node  = jsCreateTextNode;
    /* Tree walking, mutation, geometry and computed style. */
    cb.el_parent         = jsElParent;
    cb.el_child_count    = jsElChildCount;
    cb.el_child          = jsElChild;
    cb.el_is_tag         = jsElIsTag;
    cb.el_is_live        = jsElIsLive;
    cb.el_append_child   = jsElAppendChild;
    cb.el_insert_before  = jsElInsertBefore;
    cb.el_remove_child   = jsElRemoveChild;
    cb.el_clone_node     = jsElCloneNode;
    cb.el_remove_attr    = jsElRemoveAttr;
    cb.el_get_rect       = jsElGetRect;
    cb.el_get_style      = jsElGetStyle;
    cb.el_focus          = jsElFocus;
    cb.el_blur           = jsElBlur;
    cb.get_active_element = jsGetActiveElement;
    cb.get_current_script = jsGetCurrentScript;
    cb.el_get_sel        = jsElGetSel;
    cb.el_set_sel        = jsElSetSel;
    cb.el_get_dataset    = jsElGetDataset;
    cb.el_attr_snapshot  = jsElAttrSnapshot;
    cb.el_scroll_into_view = jsElScrollIntoView;
    if(!js_register_dom_natives(m_jsVm, this, &cb)) {
        EWEB_LOG("[ewebview] js: DOM native registration failed\n");
    }
    /* Canvas 2D: getContext() on Element + the CanvasRenderingContext2D class.
     * Registered after the DOM bridge so the Element class already exists.
     * `this` is the native data so getContext can reach getOrCreateCanvas. */
    registerCanvasNatives(m_jsVm);
    /* Event/EventTarget, then the window/BOM surface. Order is mandatory: both
     * look up the Document/Element/Location classes and the window/document
     * globals the DOM bridge just created (see js_event.h / js_web.h). */
    registerEventNatives(m_jsVm);
    registerWebNatives(m_jsVm);
}

void EWebEngine::resetJsVm()
{
    if(m_jsVm == nullptr) return;
    vm_close(m_jsVm);      /* frees the VM and everything it owns */
    m_jsVm = nullptr;
}

bool EWebEngine::runPageScripts()
{
    /* A page whose only JavaScript lives in inline on* attributes (onload,
     * onclick, ...) still needs a VM: those handlers are compiled on demand by
     * the event bridge and resolve against the globals it installs, so bailing
     * out on an empty script list would leave them uncallable. A page with
     * neither is skipped outright - vm_init plus four bridge registrations is
     * too much memory to spend on plain markup. */
    if(!m_jsEnabled || m_jsPageDisabled || m_buildDoc == nullptr) return true;
    if(m_jsVm == nullptr && m_jsScripts.empty() && !m_jsHasInlineHandlers) return true;
    initJsVm();
    if(m_jsVm == nullptr) return true;

    while(m_jsNextScript < m_jsScripts.size()) {
        size_t i = m_jsNextScript;
        /* Pre-paint wall clock: hand the scripts that did not run yet to the
         * post-swap phase (BUILD_SWAP_DOC re-arms BUILD_RUN_JS for them) and
         * let the build paint now. Checked before the fetch wait below too, so
         * a slow subresource cannot hold the first paint either. */
        if(m_jsPrePaintAt != 0 && (ticMs() - m_jsPrePaintAt) > kJsPrePaintBudgetMs) {
            EWEB_LOG("[ewebview] js: pre-paint budget exhausted at script %d of %d - painting first\n",
                (int)i, (int)m_jsScripts.size());
            m_jsPrePaintAt = 0;
            m_jsPrePaintCut = true;
            m_jsRunBeforePaint = false;
            return true;
        }
        /* Document order: an inline block after an external <script src> must
         * see the globals that script defines (w3.org's bootstrap news
         * FontFaceObserver from the library script ahead of it), so park the
         * run on a still-pending slot instead of skipping it. processResults
         * fills the slot and re-arms this phase via m_deferBuildStep; a fetch
         * that never resolves is skipped past the deadline exactly like a
         * 404, so the build cannot wedge. */
        if(i < m_jsScriptDone.size() && !m_jsScriptDone[i]) {
            if(m_jsScriptWaitSince == 0) m_jsScriptWaitSince = ticMs();
            if(ticMs() - m_jsScriptWaitSince < 10000) return false;
            m_jsScriptDone[i] = 1;
            if(i < m_jsScripts.size()) m_jsScripts[i].clear();
        }
        m_jsScriptWaitSince = 0;
        m_jsNextScript++;
        /* Copy by value: a script that injects another (appendChild of a
         * <script>) push_backs to m_jsScripts, which can realloc and dangle a
         * reference held across vm_load_run below. */
        std::string src = m_jsScripts[i];
        if(src.empty()) continue;
        /* A body the run budget already terminated once proved runaway on this
         * engine (obfuscated ad SDKs spin in a decode loop whose checksum never
         * matches); portals serve the same bundle from several mirrors, so
         * re-running it only buys another full budget of frozen input. */
        if(jsIsRunaway(src)) {
            EWEB_LOG("[ewebview] js: script %d skipped (runaway body, %u bytes)\n",
                (int)i, (unsigned)src.size());
            continue;
        }
        uint64_t run_start = ticMs();
        /* vm_load_run appends this script's bytecode after the previous one and
         * runs it; globals persist in vm->root across scripts, matching
         * separate <script> blocks that share one global scope. m_jsInScript
         * arms the VM step hook, which enforces the run budget and honours a
         * termination abort; window interaction stays live on its own thread
         * even through this pre-paint (document.write) script run. */
        jsVmEnter();
        m_jsCurScriptSrc = &src;
        m_jsCurScriptUrl = (i < m_jsScriptSrcs.size()) ? m_jsScriptSrcs[i] : std::string();
        {   /* Tag uncaught VM errors with the script index so a failing
             * minified bundle can be matched to its EWEB_DUMP_SCRIPTS file. */
            static char s_jsDbgTag[64];
            snprintf(s_jsDbgTag, sizeof(s_jsDbgTag), "script_%d", (int)i);
            m_jsVm->dbg_tag = s_jsDbgTag;
        }
        /* Same per-run state reset as the post-swap path (see above). */
        m_jsVm->terminated = false;
        m_jsVm->abort_run = false;
        m_jsVm->propagating_err = nullptr;
        m_jsVm->call_depth = 0;
        if(!vm_load_run(m_jsVm, src.c_str())) {
            EWEB_LOG("[ewebview] js: script %d failed to compile\n", (int)i);
            jsDumpScript(i, src, true);
        }
        else {
            jsDumpScript(i, src, false);
        }
        m_jsVm->dbg_tag = nullptr;
        m_jsCurScriptSrc = nullptr;
        m_jsCurScriptUrl.clear();
        jsVmExit();
        EWEB_LOG("[ewebview] js: script %d ran %u ms url=%s\n", (int)i,
            (uint32_t)(ticMs() - run_start),
            (i < m_jsScriptSrcs.size() && !m_jsScriptSrcs[i].empty())
                ? m_jsScriptSrcs[i].c_str() : "(inline)");
        /* The watchdog dropped this page's JS (three run-budget timeouts): stop
         * at once. The entry guard above is only evaluated once per call, so
         * without this the loop keeps spending a run budget per remaining
         * script - minutes on an ad-heavy portal - and the build never reaches
         * BUILD_RENDER_DOC, i.e. the page never paints at all. */
        if(m_jsPageDisabled) {
            EWEB_LOG("[ewebview] js: page JS dropped at script %d - skipping %d remaining\n",
                (int)i, (int)m_jsScripts.size() - (int)m_jsNextScript);
            m_jsPrePaintAt = 0;
            m_jsNextScript = m_jsScripts.size();
            break;
        }
    }
    EWEB_LOG("[ewebview] js: ran %d script(s)\n", (int)m_jsScripts.size());
    return true;
}

bool EWebEngine::runNextPageScript()
{
    /* Post-swap counterpart of runPageScripts(): the DOM on screen is the
     * active document (m_buildDoc is null, jsActiveDoc() == m_doc), and one
     * script runs per timer tick so the event loop keeps turning between
     * blocks. Same VM rules as above: a page with only inline on* handlers
     * still needs the VM, a page with neither never gets one. */
    if(!m_jsEnabled || m_jsPageDisabled || jsActiveDoc() == nullptr) return false;
    if(m_jsVm == nullptr && m_jsScripts.empty() && !m_jsHasInlineHandlers) return false;
    initJsVm();
    if(m_jsVm == nullptr) return false;

    while(m_jsNextScript < m_jsScripts.size()) {
        size_t i = m_jsNextScript;
        /* Post-swap phase wall clock: an ad-heavy portal carries dozens of
         * scripts, each able to burn a full kJsRunBudgetLiveMs, so without a
         * phase cap the engine never returns to serving input, a queued
         * navigation, or the blank-SPA notice decision - the page looks frozen
         * for tens of seconds. Past the budget stop STARTING new scripts (one
         * already in flight unwinds under its own per-run budget) and let the
         * phase complete: DOMContentLoaded/load fire and the page is usable,
         * the remaining ad scripts are simply dropped. */
        if(m_jsPostSwapAt != 0 && (ticMs() - m_jsPostSwapAt) > kJsPostSwapBudgetMs) {
            EWEB_LOG("[ewebview] js: post-swap budget %u ms exhausted at script %d of %d - dropping the rest\n",
                (unsigned)kJsPostSwapBudgetMs, (int)i, (int)m_jsScripts.size());
            m_jsNextScript = m_jsScripts.size();
            m_jsPostSwapAt = 0;
            break;
        }
        /* An external <script src> slot stays empty until its EWEB_TASK_SCRIPT
         * fetch lands (processResults fills m_jsScripts[i] and flips
         * m_jsScriptDone[i] to 1). Report "still running" so the engine keeps
         * BUILD_RUN_JS armed and re-enters us when the result wakes it - this
         * preserves document order without busy-spinning (the loop parks on a
         * 4ms tick; pushResult signals it the instant bytes arrive). A failed
         * fetch is still marked done with an empty body, so a 404 never wedges
         * the ordered run. */
        if(i < m_jsScriptDone.size() && !m_jsScriptDone[i]) return true;
        m_jsNextScript++;
        /* Copy by value: this script may inject another (appendChild of a
         * <script>), which push_backs to m_jsScripts and can realloc - a
         * reference held across vm_load_run below would then dangle. */
        std::string src = m_jsScripts[i];
        if(src.empty()) continue;
        if(jsIsRunaway(src)) {
            EWEB_LOG("[ewebview] js: script %d skipped (runaway body, %u bytes)\n",
                (int)i, (unsigned)src.size());
            continue;
        }
        uint64_t run_start = ticMs();
        /* Bracket vm_load_run so the DOM-bridge mutation callbacks push the
         * page to the screen mid-script (jsMarkLayoutDirty -> jsProgressiveFlush
         * -> engineRenderFrame): a long test script then shows its results as
         * they are produced. m_jsInScript arms the step hook (run budget +
         * abort). Window interaction is unaffected - it runs on the UI thread. */
        m_jsProgressiveActive = true;
        jsVmEnter();
        m_jsCurScriptSrc = &src;
        m_jsCurScriptUrl = (i < m_jsScriptSrcs.size()) ? m_jsScriptSrcs[i] : std::string();
        m_jsLastFlushAt = run_start;
        {
            static char s_jsDbgTag[64];
            snprintf(s_jsDbgTag, sizeof(s_jsDbgTag), "script_%d", (int)i);
            m_jsVm->dbg_tag = s_jsDbgTag;
        }
        /* A prior script may have ended abnormally INSIDE the VM (runaway
         * recursion calls vm_terminate from func_call; a cross-frame throw can
         * leave abort_run/propagating_err set). vm_run loops on !vm->terminated
         * and bails on abort_run, so any residue here makes THIS script's body
         * no-op silently - one bad script must never suppress the rest of the
         * page's JS (taobao: script_3 stack-overflow killed scripts 4..32).
         * Reset the per-run state; jsVmExit below re-detects a fresh cut. */
        m_jsVm->terminated = false;
        m_jsVm->abort_run = false;
        m_jsVm->propagating_err = nullptr;
        m_jsVm->call_depth = 0;
        if(!vm_load_run(m_jsVm, src.c_str())) {
            EWEB_LOG("[ewebview] js: script %d failed to compile\n", (int)i);
            jsDumpScript(i, src, true);
        }
        else {
            jsDumpScript(i, src, false);
        }
        m_jsVm->dbg_tag = nullptr;
        m_jsCurScriptSrc = nullptr;
        m_jsCurScriptUrl.clear();
        bool terminated = jsVmExit();
        m_jsProgressiveActive = false;
        /* A watchdog cut only proved this body cannot finish within the run
         * budget - but in document order it blocks everything behind it until
         * it burns the budget (taobao's telemetry SDK spins for minutes ahead
         * of the mtop/React bundles that render the page). Requeue the cut
         * body ONCE at the tail of the queue so the app chain gets its turn
         * first; the slow body retries last and, cut again, stays marked
         * runaway exactly as before. */
        if(terminated &&
           std::find(m_jsRequeuedSrcs.begin(), m_jsRequeuedSrcs.end(), src) ==
               m_jsRequeuedSrcs.end()) {
            m_jsRequeuedSrcs.push_back(src);
            m_jsScripts.push_back(src);
            m_jsScriptDone.push_back(1);
            for(auto it = m_jsRunawaySrcs.begin(); it != m_jsRunawaySrcs.end(); ++it) {
                if(*it == src) { m_jsRunawaySrcs.erase(it); break; }
            }
            EWEB_LOG("[ewebview] js: script %d requeued at tail (watchdog cut)\n", (int)i);
        }
        EWEB_LOG("[ewebview] js: script %d ran %u ms (post-swap) url=%s\n",
            (int)i, (uint32_t)(ticMs() - run_start),
            (i < m_jsScriptSrcs.size() && !m_jsScriptSrcs[i].empty())
                ? m_jsScriptSrcs[i].c_str() : "(inline)");
        /* Paint what this script produced before the next one runs. */
        jsProgressiveFlush(true);
        break;
    }
    return m_jsNextScript < m_jsScripts.size();
}

void EWebEngine::jsDynamicScriptInserted(void* script_el)
{
    /* ENGINE-THREAD ONLY. Called from jsElAppendChild/jsElInsertBefore when a
     * script splices a <script> element into the tree - the classic
     * createElement('script'); s.src=...; body.appendChild(s) loader pattern
     * w3.org uses for members.js. Queue its fetch (external) or body (inline)
     * as a new ordered slot and re-arm the post-swap run so it executes once
     * the script currently on the stack unwinds. */
    if(script_el == nullptr) return;
    litehtml::element* sc = (litehtml::element*)script_el;
    const char* tn = sc->get_tagName();
    EWEB_LOG("[ewebview] dynScript entry: el=%p tag=%s\n", script_el, tn != nullptr ? tn : "(null)");
    if(tn == nullptr || strcmp(tn, "script") != 0) return;

    std::string body;
    std::string srcabs;
    const char* src_attr = sc->get_attr("src", nullptr);
    EWEB_LOG("[ewebview] dynScript src_attr=%s docurl=%s\n",
        src_attr != nullptr ? src_attr : "(null)", jsDocumentUrl().c_str());
    if(src_attr != nullptr && src_attr[0] != 0) {
        /* External: resolve against the document URL and fetch it. An
         * unresolvable src is dropped so it can never block the ordered run. */
        srcabs = EWebContainer::getFullURL(&m_port, src_attr, jsDocumentUrl());
        if(srcabs.empty()) return;
    } else {
        /* Inline: the body is the element's text; skip a blank one. */
        sc->get_text(body);
        if(body.empty()) return;
    }

    /* Append a new slot. m_jsScripts holds the body (empty until an external
     * fetch fills it), m_jsScriptSrcs the absolute URL ("" for inline), and
     * m_jsScriptDone whether the body is ready (inline: now; external: not
     * until processResults lands the EWEB_TASK_SCRIPT result).
     *
     * An external CDN combo ("/??a,b,c") downloads as ONE body, so a watchdog
     * cut on a hanging middle component discards everything after it. Split it
     * into one slot + one fetch per component, exactly like the initial
     * <script src> queue does, so each part gets its own run budget. The inline
     * body (srcabs empty) passes through ewebSplitComboUrl untouched. */
    std::vector<std::string> parts = ewebSplitComboUrl(srcabs);
    for(size_t k = 0; k < parts.size(); ++k) {
        bool external = !parts[k].empty();
        m_jsScripts.push_back(external ? std::string() : body);
        m_jsScriptSrcs.push_back(parts[k]);
        m_jsScriptDone.push_back(external ? 0 : 1);
        if(external) {
            EWebTask task;
            task.url = parts[k];
            task.type = EWEB_TASK_SCRIPT;
            task.loading = false;
            addTask(task);
        }
    }

    /* Re-arm only if the run already finished (BUILD_IDLE): during a normal
     * post-swap run the loop re-reads m_jsScripts.size() and picks the new slot
     * up on its own. A script injected later (from a timer or event) needs the
     * phase restarted; m_deferBuildStep lets a just-queued fetch settle first. */
    if(m_buildPhase == BUILD_IDLE && m_jsVm != nullptr) {
        m_jsPostSwapRun = true;
        m_jsPostSwapAt = ticMs();   /* fresh budget for the re-armed phase */
        m_buildPhase = BUILD_RUN_JS;
        m_deferBuildStep = true;
    }
    EWEB_LOG("[ewebview] js: dynamic <script> queued: %s\n",
        srcabs.empty() ? "(inline)" : srcabs.c_str());
}

void EWebEngine::jsProgressiveFlush(bool force)
{
    /* ENGINE-THREAD ONLY. Called from a DOM-bridge mutation callback
     * (jsMarkLayoutDirty) while a script runs, or between scripts (force=true).
     * Its one job is to push the page's current state to the screen: re-layout
     * if dirty, then rasterize a frame and hand it to the UI (engineRenderFrame
     * -> EUET_FRAME). There is no event pumping any more - the UI thread runs
     * its own loop in parallel, so a long script can never freeze window
     * interaction. The flush is rate-limited by the measured cost of the
     * previous one so a heavy page cannot drown the run in back-to-back
     * rasterizations while a light page refreshes briskly. */
    if(!force && !m_jsProgressiveActive && !m_jsInScript) return;

    /* Only the page on screen is flushed; while a build doc exists the normal
     * BUILD_* pipeline owns rendering (the overlay is showing anyway). */
    if(m_doc == nullptr || m_buildDoc != nullptr) return;
    if(!m_needsLayout && !m_contentDirty) return;   /* nothing visible changed */

    uint64_t now = ticMs();
    if(!force) {
        /* Adaptive gap from the measured cost of the previous flush: batch
         * DOM-progress repaints (cost x 3 in 200..1500ms) so results-streaming
         * never eats the whole script run. */
        uint32_t gap = m_jsFlushCostMs * 3;
        if(gap < kJsFlushMinGapMs) gap = kJsFlushMinGapMs;
        if(gap > kJsFlushMaxGapMs) gap = kJsFlushMaxGapMs;
        if(m_jsLastFlushAt != 0 && (now - m_jsLastFlushAt) < gap) return;
    }

    uint64_t flush_start = now;
    if(m_needsLayout) {
        m_doc->render(m_clientWidth);
        m_needsLayout = false;
        m_layoutDirtyAt = 0;
        m_layoutDirtySince = 0;
        clampScrollLocked(m_doc->width(), m_doc->height());
        markContentDirty();
    }
    /* Rasterize the laid-out page into a pool buffer and post it to the UI.
     * engineRenderFrame is a no-op while a frame is still pending adoption
     * (back-pressure), so a fast script cannot outrun the display. */
    engineRenderFrame(true);

    m_jsFlushCostMs = (uint32_t)(ticMs() - flush_start);
    m_jsLastFlushAt = ticMs();
    EWEB_LOG("[ewebview] js progressive flush: %u ms force=%d\n",
        m_jsFlushCostMs, force ? 1 : 0);
}

void EWebEngine::jsDropWriteBuffer()
{
    /* Discard whatever document.write() accumulated without splicing it: the
     * pre-paint phase ended on its wall-clock budget, and splicing restarts the
     * build with a cleared script list, which would drop every script that still
     * has to run post-swap. The partial write of a page we stopped early is ad
     * scaffolding, not content. */
    if(m_jsVm == nullptr) return;
    char* buf = js_dom_take_write_buffer(m_jsVm);
    if(buf == nullptr) return;
    EWEB_LOG("[ewebview] js: document.write buffer dropped (%d byte(s)) on pre-paint cut\n",
        (int)strlen(buf));
    mario_free(buf);
}

bool EWebEngine::jsIsRunaway(const std::string& src) const
{
    for(size_t i = 0; i < m_jsRunawaySrcs.size(); ++i) {
        if(m_jsRunawaySrcs[i] == src) return true;
    }
    return false;
}

bool EWebEngine::applyJsWriteBuffer()
{
    if(m_jsVm == nullptr) return false;
    char* buf = js_dom_take_write_buffer(m_jsVm);
    if(buf == nullptr) return false;
    if(buf[0] == 0) {
        mario_free(buf);
        return false;
    }
    if(m_jsReparseCount >= kJsMaxReparse) {
        EWEB_LOG("[ewebview] js: document.write reparse cap (%d) hit, dropping %d byte(s)\n",
            kJsMaxReparse, (int)strlen(buf));
        mario_free(buf);
        return false;
    }
    m_jsReparseCount++;
    std::string written(buf);
    mario_free(buf);

    /* Splice the written markup in just before </body> (append if absent) so it
     * is parsed on restart. Scripts are cleared so the rebuilt page does not
     * re-run and re-write forever; the reparse counter is a second guard. */
    std::string html = m_buildHtmlContent;
    std::string lower = html;
    for(char& ch : lower) ch = (char)::tolower((unsigned char)ch);
    /* The minimal std::string in use has find() but no rfind(); a well-formed
     * page has a single </body>, so the first match is the insertion point. */
    size_t body_close = lower.find("</body>");
    if(body_close != std::string::npos) {
        html.insert(body_close, written);
    } else {
        html += written;
    }
    m_buildHtmlContent = html;

    /* The delete below is what makes every cached element handle and every
     * listener registered against one dangle, so drop them first. The VM
     * itself survives (it re-runs the spliced page's scripts from scratch) but
     * its bridge state must not outlive the document it points into. */
    jsInvalidateHandles();
    if(m_buildDoc) { delete m_buildDoc; m_buildDoc = nullptr; }
    if(m_buildContainer) { delete m_buildContainer; m_buildContainer = nullptr; }
    m_jsScripts.clear();
    m_jsCurScriptEl = nullptr;
    m_jsCurScriptElUrl.clear();
    EWEB_LOG("[ewebview] js: document.write spliced %d byte(s), reparse #%d\n",
        (int)written.size(), m_jsReparseCount);
    return true;
}

/* ==================================================================
 * VM run watchdog (see kJsRunBudgetMs / kJsRunAbortMax in EWebInternal.h)
 * ================================================================== */

void EWebEngine::jsVmEnter()
{
    /* Wall-clock anchor for the step-hook budget. m_jsInScript marks that a VM
     * run is in flight on the engine thread; a navigation requested from inside
     * it is deferred until it unwinds (see jsRunPendingNavigation). The abort
     * generation is snapshotted so jsOnVmStep can tell an abort raised DURING
     * this run apart from a stale flag left by a STOP consumed while idle. */
    m_jsEnterAt = ticMs();
    /* The pre-paint phase answers to its own wall clock, so a single run may
     * keep the (larger) legacy budget; every run against a live page gets the
     * short one, because the engine thread cannot serve input while it lasts. */
    m_jsRunDeadline = m_jsEnterAt +
        ((m_jsPrePaintAt != 0) ? kJsRunBudgetMs : kJsRunBudgetLiveMs);
    m_jsEnterGen = m_buildAbortGen;
    m_jsAbortPrePaint = false;
    m_jsInScript = true;
}

bool EWebEngine::jsVmExit()
{
    m_jsInScript = false;
    m_jsEnterAt = 0;
    if(m_jsVm == nullptr || !m_jsVm->terminated)
        return false;
    /* The step hook terminated this run: drop the operands/scopes the unwound
     * vm_run frames left behind (same cleanup vm_close relies on) so the VM can
     * serve the next callback, and clear the flag vm_run loops on. */
    vm_terminate(m_jsVm);
    m_jsVm->terminated = false;
    if(m_jsAbortPrePaint) {
        /* The pre-paint wall clock ran out, not this script: the page's JS is
         * fine, the build just has to stop deferring the first paint. Charging
         * it to the watchdog would drop a working page's scripts after three. */
        EWEB_LOG("[ewebview] js: pre-paint budget %u ms exhausted - deferring the rest post-swap\n",
             (unsigned)kJsPrePaintBudgetMs);
        return true;
    }
    /* An abort raised by a termination/navigation request DURING this run (the
     * generation moved past the jsVmEnter snapshot) is intentional and must NOT
     * count against the page's watchdog budget; only a genuine run-budget
     * timeout does. A stale flag (STOP consumed while this page sat idle) has
     * the same generation as the snapshot, so runs after it count normally. */
    if(m_buildAbortGen == m_jsEnterGen) {
        m_jsAbortCount++;
        EWEB_LOG("[ewebview] js: script run aborted by watchdog (%d/%d)\n",
             m_jsAbortCount, kJsRunAbortMax);
        /* Remember the body so its mirrors/replicas are not run again: one
         * terminated run already proved it cannot finish on this engine. */
        if(m_jsCurScriptSrc != nullptr && !m_jsCurScriptSrc->empty())
            m_jsRunawaySrcs.push_back(*m_jsCurScriptSrc);
        if(m_jsAbortCount >= kJsRunAbortMax) {
            EWEB_LOG("[ewebview] js: too many aborted runs - dropping this page's JS\n");
            m_jsPageDisabled = true;
        }
    }
    return true;
}

void EWebEngine::jsVmStepHook(struct st_vm* vm, void* data)
{
    ((EWebEngine*)data)->jsOnVmStep(vm);
}

void EWebEngine::jsOnVmStep(struct st_vm* vm)
{
    if(vm == nullptr || vm->terminated)
        return;
    /* A termination request (stop / back / reload / exit) bumps m_buildAbortGen
     * from the UI thread; if the generation moved past this run's jsVmEnter
     * snapshot the abort arrived DURING this run, so unwind at once instead of
     * running to the watchdog budget. An unchanged generation means any raised
     * m_buildAbort is stale (a STOP consumed while the page sat idle) and this
     * run belongs to the still-visible page: let it proceed. */
    if(m_buildAbortGen != m_jsEnterGen) {
        vm->terminated = true;
        return;
    }
    uint64_t now = ticMs();
    if(m_jsRunDeadline != 0 && now > m_jsRunDeadline) {
        EWEB_LOG("[ewebview] js: run budget %u ms exceeded - terminating\n",
             (unsigned)(m_jsRunDeadline - m_jsEnterAt));
        vm->terminated = true;   /* unwind every nested vm_run frame */
        return;
    }
    /* Pre-paint pages additionally answer to the phase wall clock, so ONE slow
     * script cannot spend the whole run budget before the first paint: the
     * cut is flagged so jsVmExit does not charge it to the watchdog. */
    if(m_jsPrePaintAt != 0 && (now - m_jsPrePaintAt) > kJsPrePaintBudgetMs) {
        m_jsAbortPrePaint = true;
        vm->terminated = true;
        return;
    }
}

int EWebEngine::jsPollTimers()
{
    /* Engine loop step 7: setInterval()/setTimeout()/requestAnimationFrame().
     * The guard mirrors the original loop body; js_dom_poll_timers returns how
     * many callbacks fired (a firing may have drawn without touching layout,
     * so the caller marks the content dirty). */
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled) return 0;
    jsVmEnter();
    int fired = js_dom_poll_timers(m_jsVm, ticMs());
    jsVmExit();
    return fired;
}

/* ==================================================================
 * Core DOM bridge callbacks (see js_dom.h for the contract)
 * ================================================================== */

void EWebEngine::jsAlert(void* ctx, const char* msg)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return;
    /* Non-blocking: surface the text to the UI thread (on_dialog). A modal
     * dialog would need a cross-thread round-trip the engine must never wait
     * on. */
    EWebUiEvent ev;
    ev.kind = EUET_DIALOG;
    ev.text = (msg != nullptr) ? msg : "";
    self->postUiEvent(ev);
    EWEB_LOG("[ewebview] js alert: %s\n", ev.text.c_str());
}

void EWebEngine::jsDocumentWrite(void* ctx, const char* html)
{
    (void)ctx;
    /* The natives also accumulate writes into their own buffer, drained by
     * applyJsWriteBuffer(); this hook only traces each write as it happens. */
    if(html != nullptr) {
        EWEB_LOG("[ewebview] js document.write: %d byte(s)\n", (int)strlen(html));
    }
}

char* EWebEngine::jsGetTitle(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    /* jsActiveDoc(), not m_buildDoc: document.title is read by timer callbacks
     * long after BUILD_SWAP_DOC has nulled the build pointer. */
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    litehtml::element::ptr t = root->select_one("title");
    if(t == nullptr) return nullptr;
    std::string text;
    t->get_text(text);
    return js_strdup_mario(text.c_str());
}

void EWebEngine::jsSetTitle(void* ctx, const char* title)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return;
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return;
    litehtml::element::ptr t = root->select_one("title");
    if(t == nullptr) return;   /* no <title> to update; ignore (MVP) */
    js_set_element_text(t, title != nullptr ? title : "", &self->m_jsDetached);
    self->recordJsMutation(2, "", "", title != nullptr ? title : "");
    self->jsMarkLayoutDirty();
    /* Surface the new title to the embedder (the widget original had no
     * window-title hook and dropped it; the public listener has on_title). */
    EWebUiEvent ev;
    ev.kind = EUET_TITLE;
    ev.text = (title != nullptr) ? title : "";
    self->postUiEvent(ev);
}

char* EWebEngine::jsGetUrl(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    /* m_buildHtmlUrl is cleared by BUILD_SWAP_DOC, but location.href keeps
     * working for the whole life of the page, so fall back to the URL the
     * visible document was loaded from (jsDocumentUrl does exactly that). */
    return js_strdup_mario(self->jsDocumentUrl().c_str());
}

void* EWebEngine::jsGetElementById(void* ctx, const char* id)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || id == nullptr) return nullptr;
    /* jsActiveDoc() for the same reason as jsGetTitle: getElementById is the
     * workhorse of every timer callback and event listener, both of which run
     * after the build doc has been swapped out and freed. */
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    /* js_dom.c routes document.body through get_element_by_id(ctx, "body"). */
    if(strcmp(id, "body") == 0) {
        litehtml::element::ptr b = root->select_one("body");
        if(b != nullptr) return (void*)b;
    }
    std::string sel = std::string("#") + id;
    litehtml::element::ptr el = root->select_one(sel.c_str());
    return (void*)el;   /* nullptr when not found -> JS null */
}

char* EWebEngine::jsElGetText(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    std::string text;
    e->get_text(text);
    return js_strdup_mario(text.c_str());
}

void EWebEngine::jsElSetText(void* ctx, void* el, const char* text)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr) return;
    js_set_element_text((litehtml::element*)el, text != nullptr ? text : "",
                        self != nullptr ? &self->m_jsDetached : nullptr);
    if(self != nullptr) {
        const char* idv = ((litehtml::element*)el)->get_attr("id", nullptr);
        if(idv != nullptr && idv[0] != 0)
            self->recordJsMutation(0, idv, "", text != nullptr ? text : "");
        self->jsMarkLayoutDirty();
    }
}

char* EWebEngine::jsElGetHtml(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return nullptr;
    /* litehtml has no innerHTML serializer; return the text content (MVP). */
    litehtml::element* e = (litehtml::element*)el;
    std::string text;
    e->get_text(text);
    return js_strdup_mario(text.c_str());
}

void EWebEngine::jsElSetHtml(void* ctx, void* el, const char* html)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    std::string frag(html != nullptr ? html : "");
    EWEB_LOG("[ewebview] jsElSetHtml ENTER el=%p tag=%s len=%d\n", el,
             e->get_tagName() != nullptr ? e->get_tagName() : "(null)", (int)frag.size());
    /* Real fragment parse (this used to be a tag-stripping MVP stub): markup
     * assigned to innerHTML now builds actual elements, so an injected <img>
     * fetches and renders instead of being flattened to text. */
    if(self != nullptr && e->get_document() != nullptr) {
        js_apply_inner_html(e, frag, &self->m_jsDetached);
        const char* idv = e->get_attr("id", nullptr);
        if(idv != nullptr && idv[0] != 0)
            self->recordJsMutation(3, idv, "", frag);   /* kind 3: innerHTML (replay re-parses) */
        self->jsMarkLayoutDirty();
    } else {
        /* No document to parse against: fall back to the text-only setter. */
        js_set_element_text(e, js_strip_tags(frag).c_str(),
                            self != nullptr ? &self->m_jsDetached : nullptr);
    }
}

char* EWebEngine::jsElGetAttr(void* ctx, void* el, const char* name)
{
    (void)ctx;
    if(el == nullptr || name == nullptr) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    /* Form controls expose their LIVE state: .value / .checked (and
     * getAttribute on them) read the widget's single source of truth, so what
     * the user typed or ticked is exactly what scripts observe. */
    void* w = e->eweb_form_widget();
    if(w != nullptr) {
        eweb_el_input* wi = (eweb_el_input*)w;
        if(strcmp(name, "value") == 0)
            return js_strdup_mario(wi->value().c_str());
        if(strcmp(name, "checked") == 0)
            return wi->isChecked() ? js_strdup_mario("") : nullptr;
    }
    const char* v = e->get_attr(name, nullptr);
    if(v == nullptr) return nullptr;
    return js_strdup_mario(v);
}

/* JS-driven changes to class/id alter which selectors match, and a style=
 * change alters the inline cascade: a layout-only invalidation would repaint
 * the OLD resolved values. Re-match the subtree against the sheets (budgeted
 * inside litehtml) and re-resolve used styles so class toggles (".chip.on"
 * and friends) actually paint. ENGINE-THREAD ONLY (VM callback context). */
static void jsRestyleSubtree(litehtml::element* e, bool recascade)
{
    if(e == nullptr) return;
    if(recascade) {
        litehtml::document* doc = e->get_document();
        if(doc != nullptr) doc->style_detached_subtree(e);
    }
    e->parse_styles(true);
}

void EWebEngine::jsElSetAttr(void* ctx, void* el, const char* name, const char* value)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr || name == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    /* Writing .value / .checked goes to the widget so the edit buffer and the
     * drawn text stay the single source of truth (setValue re-syncs the
     * attribute itself); every other name keeps plain attribute behaviour. */
    bool routed = false;
    void* w = e->eweb_form_widget();
    if(w != nullptr) {
        eweb_el_input* wi = (eweb_el_input*)w;
        if(strcmp(name, "value") == 0) {
            wi->setValue(value != nullptr ? value : "");
            routed = true;
        } else if(strcmp(name, "checked") == 0) {
            wi->setChecked(true);
            routed = true;
        }
    }
    if(!routed) {
        e->set_attr(name, value != nullptr ? value : "");
        /* class/id decide selector matching; style= feeds the inline cascade.
         * Neither is visible to a layout-only invalidation, so restyle. */
        if(strcmp(name, "class") == 0 || strcmp(name, "id") == 0)
            jsRestyleSubtree(e, true);
        else if(strcmp(name, "style") == 0)
            jsRestyleSubtree(e, false);
    }
    if(self != nullptr) {
        const char* idv = e->get_attr("id", nullptr);
        if(idv != nullptr && idv[0] != 0)
            self->recordJsMutation(1, idv, name, value != nullptr ? value : "");
        self->jsMarkLayoutDirty();
    }
}

char* EWebEngine::jsElGetTag(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    const char* t = e->get_tagName();
    return js_strdup_mario(t != nullptr ? t : "");
}

/* ==================================================================
 * Extended DOM bridge callbacks (querySelector, tree walking/mutation,
 * geometry, computed style)
 * ================================================================== */

void* EWebEngine::jsGetRoot(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;
    return (void*)doc->root();
}

/* <body> and <head> are located by selector rather than by a dedicated
 * litehtml accessor: document exposes only root(), and is_body() would mean
 * walking the tree by hand. select_one() is the same call jsGetElementById()
 * already relies on. */
void* EWebEngine::jsGetBody(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) { EWEB_LOG("jsGetBody: no active doc"); return nullptr; }
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) { EWEB_LOG("jsGetBody: no root"); return nullptr; }
    void* b = (void*)root->select_one("body");
    if(b == nullptr) EWEB_LOG("jsGetBody: body NOT FOUND in tree");
    return b;
}

void* EWebEngine::jsGetHead(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    return (void*)root->select_one("head");
}

/* document.currentScript: the <script> element whose body is executing right
 * now. Security SDKs (taobao's baxia) insert their loader next to themselves
 * via currentScript.parentNode.insertBefore(...); without the property they
 * fall back to parsing Error().stack, whose format never matches here, so the
 * SDK dereferences null and never installs its request signer. */
void* EWebEngine::jsGetCurrentScript(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    void* r = self->jsCurrentScriptEl();
    EWEB_LOG("[ewebview] currentScript: url=%s -> %p\n",
        self->m_jsCurScriptUrl.c_str(), r);
    return r;
}

void* EWebEngine::jsCurrentScriptEl()
{
    if(m_jsCurScriptUrl.empty()) return nullptr;
    litehtml::document* doc = jsActiveDoc();
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    /* Compare scheme-less: markup often carries "//g.alicdn.com/..." while the
     * run queue stores the resolved absolute URL. */
    auto norm = [](const std::string& u) -> std::string {
        if(u.compare(0, 6, "https:") == 0) return u.substr(6);
        if(u.compare(0, 5, "http:") == 0) return u.substr(5);
        return u;
    };
    std::string want = norm(m_jsCurScriptUrl);
    size_t n = root->get_children_count();
    for(size_t i = 0; i < n; ++i) {
        litehtml::element::ptr c = root->get_child((int)i);
        if(c == nullptr) continue;
        litehtml::elements_vector part = c->select_all(litehtml::tstring("script"));
        for(size_t j = 0; j < part.size(); ++j) {
            if(part[j] == nullptr) continue;
            const char* sa = part[j]->get_attr("src", nullptr);
            if(sa == nullptr || sa[0] == 0) continue;
            if(norm(std::string(sa)) == want) return (void*)part[j];
        }
    }
    /* Static <script src> tags are stripped from the markup before parsing
     * (their bodies run from the ordered queue), so the tree holds no element
     * for them. Materialise a stand-in in <head> carrying the same src: SDKs
     * only use currentScript to splice their loader next to it, and a real
     * attached element is what makes parentNode.insertBefore() work. Cached
     * per URL so repeated reads see one stable node. */
    if(m_jsCurScriptEl != nullptr && m_jsCurScriptElUrl == m_jsCurScriptUrl)
        return m_jsCurScriptEl;
    litehtml::string_map attrs;
    litehtml::element::ptr el = doc->create_element("script", attrs);
    if(el == nullptr) return nullptr;
    el->set_attr("src", m_jsCurScriptUrl.c_str());
    litehtml::element::ptr host = root->select_one("head");
    if(host == nullptr) host = root->select_one("body");
    if(host == nullptr) return nullptr;
    host->appendChild(el);
    m_jsCurScriptEl = (void*)el;
    m_jsCurScriptElUrl = m_jsCurScriptUrl;
    EWEB_LOG("[ewebview] currentScript: stand-in created el=%p host=%s\n",
        m_jsCurScriptEl, (root->select_one("head") != nullptr) ? "head" : "body");
    return m_jsCurScriptEl;
}

int EWebEngine::jsQueryAll(void* ctx, void* root, const char* selector,
                           int skip, void** out, int max)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(selector == nullptr || selector[0] == 0 || out == nullptr || max <= 0)
        return 0;

    litehtml::element* base = (litehtml::element*)root;
    if(base == nullptr) {
        /* NULL root means "the whole document". */
        if(self == nullptr) return 0;
        litehtml::document* doc = self->jsActiveDoc();
        if(doc == nullptr) return 0;
        base = doc->root();
        if(base == nullptr) return 0;
    }

    /* litehtml's select_all() matches the element it is called on as well as
     * its descendants, while querySelector/querySelectorAll must never match
     * the context node itself. Start from the children to get DOM semantics. */
    litehtml::tstring sel(selector);
    litehtml::elements_vector hits;
    size_t n = base->get_children_count();
    for(size_t i = 0; i < n; ++i) {
        litehtml::element::ptr c = base->get_child((int)i);
        if(c == nullptr) continue;
        litehtml::elements_vector part = c->select_all(sel);
        for(size_t j = 0; j < part.size(); ++j) {
            if(part[j] != nullptr) hits.push_back(part[j]);
        }
    }

    /* `skip` lets the bridge page through more matches than fit in `out`; see
     * the js_dom_callbacks_t comment. Returning exactly `max` is the bridge's
     * cue to call again with a higher skip. */
    int written = 0;
    for(size_t i = 0; i < hits.size(); ++i) {
        if(skip > 0) { skip--; continue; }
        if(written >= max) break;
        out[written++] = (void*)hits[i];
    }
    return written;
}

void* EWebEngine::jsCreateElement(void* ctx, const char* tag)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || tag == nullptr || tag[0] == 0) return nullptr;
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;

    /* document::create_element dispatches on the lower-cased tag name to pick
     * the right subclass (el_image, el_anchor, el_table, ...), so normalise
     * first - createElement("DIV") must build the same node as "div". */
    std::string lower(tag);
    for(size_t i = 0; i < lower.size(); ++i)
        lower[i] = (char)tolower((unsigned char)lower[i]);

    /* Empty attribute map: attributes arrive through setAttribute(), which the
     * bridge routes to el_set_attr. */
    litehtml::string_map attrs;
    litehtml::element::ptr el = doc->create_element(lower.c_str(), attrs);
    return (void*)el;
}

void* EWebEngine::jsCreateTextNode(void* ctx, const char* text)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;

    /* el_text is what litehtml itself builds for character data. Constructed
     * the way litehtml_alloc does (malloc + placement new) so that the
     * `delete` in jsElRemoveChild / jsFreeDetachedNodes resolves to free(),
     * matching how html_tag's destructor releases its children. */
    void* mem = malloc(sizeof(litehtml::el_text));
    if(mem == nullptr) return nullptr;
    litehtml::el_text* t = new (mem) litehtml::el_text(text != nullptr ? text : "", doc);
    return (void*)t;
}

void* EWebEngine::jsElCloneNode(void* ctx, void* el, int deep)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr) return nullptr;
    /* Reject a dangling handle before dereferencing (see jsElAppendChild). */
    if(!jsElIsLive(ctx, el)) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    /* clone_node recreates the tag + attributes through the document factory
     * (deep: children too) and returns an unstyled, detached node. Styles are
     * matched again when the clone is spliced in (appendChild/insertBefore ->
     * style_detached_subtree + parse_styles), exactly like a createElement'd
     * node. */
    litehtml::element::ptr cp = e->clone_node(deep != 0);
    EWEB_LOG("[ewebview] jsElCloneNode el=%p deep=%d -> cp=%p\n", el, deep, (void*)cp);
    if(cp == nullptr) return nullptr;
    /* Park the clone so jsFreeDetachedNodes() reclaims it at teardown if the
     * script never inserts it; js_unpark() drops it once it goes back in. */
    if(self != nullptr) self->m_jsDetached.push_back(cp);
    return (void*)cp;
}

void* EWebEngine::jsElParent(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return nullptr;
    return (void*)((litehtml::element*)el)->parent();
}

/* Pseudo-elements (::before/::after) are layout artifacts of litehtml, not DOM
 * nodes: the spec keeps them out of childNodes/children/firstChild. Exposing
 * them made w3.org's convertLinkToButton() treat the link's ::before as its
 * first child and shuffle the label text into the wrong node. */
static bool jsElIsPseudo(void* el)
{
    if(el == nullptr) return false;
    const char* t = ((litehtml::element*)el)->get_tagName();
    return (t != nullptr && t[0] == ':' && t[1] == ':');
}

int EWebEngine::jsElChildCount(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return 0;
    litehtml::element* e = (litehtml::element*)el;
    int n = 0;
    for(size_t i = 0; i < e->get_children_count(); i++)
    {
        if(!jsElIsPseudo((void*)e->get_child(i))) n++;
    }
    return n;
}

void* EWebEngine::jsElChild(void* ctx, void* el, int idx)
{
    (void)ctx;
    if(el == nullptr || idx < 0) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    int seen = 0;
    for(size_t i = 0; i < e->get_children_count(); i++)
    {
        void* c = (void*)e->get_child(i);
        if(jsElIsPseudo(c)) continue;
        if(seen == idx) return c;
        seen++;
    }
    return nullptr;
}

bool EWebEngine::jsElIsTag(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return false;
    /* Only html_tag and its subclasses carry a tag name; el_text and
     * el_comment inherit element::get_tagName(), which returns "". That is the
     * distinction childNodes (everything) vs children (elements only) needs,
     * and it avoids a dynamic_cast - the build uses -fno-rtti. */
    const char* t = ((litehtml::element*)el)->get_tagName();
    if(t == nullptr || t[0] == 0) return false;
    if(t[0] == ':' && t[1] == ':') return false; /* pseudo: not an element */
    return true;
}

bool EWebEngine::jsElIsLive(void* ctx, void* el)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr) return false;
    /* A JS Element wrapper can outlive the litehtml element it wrapped: the node
     * is freed on a document swap or an innerHTML/subtree rewrite while the
     * script still holds the wrapper, and the mario VM then recycles that memory
     * for its own objects (a stale handle has shown up pointing at a JS
     * "prototype" blob). The bridge calls this before dereferencing any handle,
     * so a dangling pointer degrades to null instead of being spliced into the
     * tree and aborting in the style walk. The plausibility test filters
     * pointers that are not mapped at all WITHOUT reading through them (the
     * port's optional sys.ptr_sane hook); the liveness tag then confirms the
     * memory still holds a live element (see litehtml's element.h). */
    if(self != nullptr && self->m_port.sys.ptr_sane != nullptr &&
            !self->m_port.sys.ptr_sane(self->m_port.sys.ud, el))
        return false;
    return ((litehtml::element*)el)->is_live_handle();
}

bool EWebEngine::jsElAppendChild(void* ctx, void* parent, void* child)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(parent == nullptr || child == nullptr) return false;
    EWEB_LOG("[ewebview] appendChild cb: parent=%p child=%p\n", parent, child);
    /* Reject a dangling handle at the boundary. A JS Element wrapper can outlive
     * the node it wrapped (freed on a document swap or innerHTML rewrite) and
     * mario recycles that memory for its own objects. element_arg() gates on
     * el_is_live, but mutations can also reach here through the deferred replay
     * path, so guard here too: a stale pointer must never be cast to a
     * litehtml::element, spliced into the tree and walked by
     * style_detached_subtree(). */
    if(!jsElIsLive(ctx, parent) || !jsElIsLive(ctx, child)) {
        EWEB_LOG("[ewebview] appendChild rejected stale handle: parent=%p child=%p\n", parent, child);
        return false;
    }
    litehtml::element* c = (litehtml::element*)child;
    if(!((litehtml::element*)parent)->appendChild(c)) return false;
    /* A node built by createElement never went through the document-creation
     * stylesheet walks, so match master/attribute/document styles against the
     * inserted subtree first - without it a scripted <div> keeps html_tag's
     * default inline display and stacks sideways. Already styled nodes (a
     * moved subtree) are skipped inside. */
    if(litehtml::document* d = c->get_document()) {
        d->style_detached_subtree(c);
    }
    /* A node built by createElement has no box yet; parse_styles measures it so
     * the next render lays it out instead of collapsing it to 0x0. This is the
     * same step js_set_element_text() takes for a fresh el_text. */
    c->parse_styles(false);
    if(self != nullptr) {
        js_unpark(self->m_jsDetached, c);   /* re-inserting a removed node */
        /* A dynamically injected <script> (appendChild of a node built by
         * createElement) must fetch and run just like a parser-inserted one;
         * the hook queues it and re-arms the post-swap run. */
        const char* tn = c->get_tagName();
        EWEB_LOG("[ewebview] appendChild cb tag=%s\n", tn != nullptr ? tn : "(null)");
        if(tn != nullptr && strcmp(tn, "script") == 0)
            self->jsDynamicScriptInserted(c);
        self->jsMarkLayoutDirty();
    }
    return true;
}

bool EWebEngine::jsElInsertBefore(void* ctx, void* parent, void* child, void* ref)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(parent == nullptr || child == nullptr) return false;
    /* See jsElAppendChild: reject dangling handles before touching the tree. A
     * stale `ref` degrades to append (null) rather than being dereferenced. */
    if(!jsElIsLive(ctx, parent) || !jsElIsLive(ctx, child)) {
        EWEB_LOG("[ewebview] insertBefore rejected stale handle: parent=%p child=%p\n", parent, child);
        return false;
    }
    if(ref != nullptr && !jsElIsLive(ctx, ref)) {
        EWEB_LOG("[ewebview] insertBefore dropped stale ref=%p (append instead)\n", ref);
        ref = nullptr;
    }
    litehtml::element* c = (litehtml::element*)child;
    /* html_tag::insertBefore appends when ref is null or is not a child of
     * parent, which is exactly the DOM contract. */
    if(!((litehtml::element*)parent)->insertBefore(c, (litehtml::element*)ref))
        return false;
    if(litehtml::document* d = c->get_document()) {
        d->style_detached_subtree(c);
    }
    c->parse_styles(false);
    if(self != nullptr) {
        js_unpark(self->m_jsDetached, c);
        /* See jsElAppendChild: a <script> spliced in via insertBefore fetches
         * and runs the same way. */
        const char* tn = c->get_tagName();
        if(tn != nullptr && strcmp(tn, "script") == 0)
            self->jsDynamicScriptInserted(c);
        self->jsMarkLayoutDirty();
    }
    return true;
}

bool EWebEngine::jsElRemoveChild(void* ctx, void* parent, void* child)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(parent == nullptr || child == nullptr) return false;
    /* See jsElAppendChild: never deref a dangling handle. */
    if(!jsElIsLive(ctx, parent) || !jsElIsLive(ctx, child)) {
        EWEB_LOG("[ewebview] removeChild rejected stale handle: parent=%p child=%p\n", parent, child);
        return false;
    }
    litehtml::element* c = (litehtml::element*)child;
    if(!((litehtml::element*)parent)->removeChild(c)) return false;
    if(self != nullptr) {
        /* removeChild only unlinks; the node is not deleted. See the comment on
         * m_jsDetached: freeing it here would dangle every cached wrapper and
         * listener keyed on this handle, so park it until the page goes away. */
        js_unpark(self->m_jsDetached, c);   /* in case it was removed twice */
        self->m_jsDetached.push_back(c);
        self->jsMarkLayoutDirty();
    }
    return true;
}

void EWebEngine::jsElRemoveAttr(void* ctx, void* el, const char* name)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr || name == nullptr) return;
    /* el.checked = false reaches the bridge as removeAttribute("checked");
     * route it to the widget so the live tick state follows the script. */
    void* w = ((litehtml::element*)el)->eweb_form_widget();
    if(w != nullptr && strcmp(name, "checked") == 0)
        ((eweb_el_input*)w)->setChecked(false);
    else {
        litehtml::element* e = (litehtml::element*)el;
        e->remove_attr(name);
        /* Same restyle contract as jsElSetAttr: dropping class/id/style can
         * change which rules match or what the inline cascade contributes. */
        if(strcmp(name, "class") == 0 || strcmp(name, "id") == 0)
            jsRestyleSubtree(e, true);
        else if(strcmp(name, "style") == 0)
            jsRestyleSubtree(e, false);
    }
    if(self != nullptr) self->jsMarkLayoutDirty();
}

void EWebEngine::jsElGetRect(void* ctx, void* el, int* x, int* y, int* w, int* h)
{
    (void)ctx;
    if(x) *x = 0;
    if(y) *y = 0;
    if(w) *w = 0;
    if(h) *h = 0;
    if(el == nullptr) return;
    /* get_placement() is the border box in document coordinates - the same
     * space getBoundingClientRect() reports before the viewport offset is
     * applied, which is what the bridge wants (it subtracts the scroll
     * offsets itself when it needs client coordinates). */
    litehtml::position p = ((litehtml::element*)el)->get_placement();
    if(x) *x = p.x;
    if(y) *y = p.y;
    if(w) *w = p.width;
    if(h) *h = p.height;
}

char* EWebEngine::jsElGetStyle(void* ctx, void* el, const char* prop)
{
    (void)ctx;
    if(el == nullptr || prop == nullptr || prop[0] == 0) return nullptr;
    /* get_style_property resolves the used value, walking the inherited chain
     * when the property is an inherited one, so it doubles as the backing
     * store for both element.style reads and getComputedStyle(). Non-const on
     * html_tag because it fills a per-element property cache. */
    const char* v = ((litehtml::element*)el)->get_style_property(prop, true, nullptr);
    if(v == nullptr || v[0] == 0) return nullptr;
    return js_strdup_mario(v);
}

void EWebEngine::jsElFocus(void* ctx, void* el)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || el == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    /* Only focusable elements take focus: form widgets, <a href>, or anything
     * carrying a tabindex. focus() on anything else is a no-op, exactly as in
     * a browser (a bare <div> is not focusable). */
    bool focusable = false;
    void* w = e->eweb_form_widget();
    if(w != nullptr) {
        focusable = ((eweb_el_input*)w)->isFocusable();
    } else {
        const litehtml::tchar_t* tag = e->get_tagName();
        const litehtml::tchar_t* href = e->get_attr("href", nullptr);
        if(tag != nullptr && tag[0] == 'a' && tag[1] == 0 &&
                href != nullptr && href[0] != 0)
            focusable = true;
        else if(e->get_attr("tabindex", nullptr) != nullptr)
            focusable = true;
    }
    if(focusable) self->setFocus(e);
}

void EWebEngine::jsElBlur(void* ctx, void* el)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || el == nullptr) return;
    /* blur() only affects the element that currently holds focus. */
    if((litehtml::element*)el == (litehtml::element*)self->m_focusElement)
        self->clearFocus();
}

/* The widget keeps caret/selection as BYTE offsets; the DOM reports codepoint
 * offsets, so both directions walk the UTF-8 of the live value. */
static int ewebUtf8Step(const std::string& t, int i)
{
    unsigned char c = (unsigned char)t[i];
    return (c & 0x80) == 0 ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
           ((c & 0xF0) == 0xE0) ? 3 : 4;
}

static int ewebBytesToCps(const std::string& t, int bytes)
{
    if(bytes > (int)t.size()) bytes = (int)t.size();
    int n = 0, i = 0;
    while(i < bytes) { i += ewebUtf8Step(t, i); n++; }
    return n;
}

static int ewebCpsToBytes(const std::string& t, int cps)
{
    int i = 0, n = 0;
    while(i < (int)t.size() && n < cps) { i += ewebUtf8Step(t, i); n++; }
    return i;
}

bool EWebEngine::jsElGetSel(void* ctx, void* el, int* s, int* e)
{
    (void)ctx;
    if(el == nullptr) return false;
    void* w = ((litehtml::element*)el)->eweb_form_widget();
    if(w == nullptr || !((eweb_el_input*)w)->isTextEditing()) return false;
    eweb_el_input* wi = (eweb_el_input*)w;
    std::string t = wi->value();
    if(s) *s = ewebBytesToCps(t, wi->selStart());
    if(e) *e = ewebBytesToCps(t, wi->selEnd());
    return true;
}

void EWebEngine::jsElSetSel(void* ctx, void* el, int s, int e)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr) return;
    void* w = ((litehtml::element*)el)->eweb_form_widget();
    if(w == nullptr || !((eweb_el_input*)w)->isTextEditing()) return;
    eweb_el_input* wi = (eweb_el_input*)w;
    std::string t = wi->value();
    wi->setSelectionRange(ewebCpsToBytes(t, s), ewebCpsToBytes(t, e));
    if(self != nullptr) self->markContentDirty();
}

char* EWebEngine::jsElGetDataset(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return nullptr;
    const litehtml::string_map* am = ((litehtml::element*)el)->eweb_attrs();
    if(am == nullptr) return nullptr;

    /* Seed the JS-side dataset object: every data-* attribute with its key
     * camelCased (data-my-id -> myId), packed as "key\x1fvalue\x1e". */
    std::string out;
    for(litehtml::string_map::const_iterator it = am->begin(); it != am->end(); ++it) {
        const std::string& k = it->first;
        if(k.compare(0, 5, "data-") != 0 || k.size() <= 5) continue;
        std::string ck;
        bool up = false;
        for(size_t i = 5; i < k.size(); i++) {
            char c = k[i];
            if(c == '-') { up = true; continue; }
            ck += up ? (char)toupper((unsigned char)c) : c;
            up = false;
        }
        if(ck.empty()) continue;
        out += ck;
        out += '\x1f';
        out += it->second;
        out += '\x1e';
    }
    if(out.empty()) return nullptr;
    char* r = (char*)mario_malloc(out.size() + 1);
    if(r == nullptr) return nullptr;
    memcpy(r, out.c_str(), out.size() + 1);
    return r;
}

char* EWebEngine::jsElAttrSnapshot(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return nullptr;
    const litehtml::string_map* am = ((litehtml::element*)el)->eweb_attrs();
    if(am == nullptr) return nullptr;

    /* Element.attributes seed: every attribute packed as
     * "name\x1fvalue\x1e"; the DOM bridge expands it into a NamedNodeMap. */
    std::string out;
    for(litehtml::string_map::const_iterator it = am->begin(); it != am->end(); ++it) {
        out += it->first;
        out += '\x1f';
        out += it->second;
        out += '\x1e';
    }
    if(out.empty()) return nullptr;
    char* r = (char*)mario_malloc(out.size() + 1);
    if(r == nullptr) return nullptr;
    memcpy(r, out.c_str(), out.size() + 1);
    return r;
}

void* EWebEngine::jsGetActiveElement(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    if(self->m_focusElement != nullptr) return self->m_focusElement;
    /* Nothing focused: activeElement is <body> (falling back to the root). */
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    litehtml::element::ptr body = root->select_one("body");
    return (body != nullptr) ? (void*)body : (void*)root;
}

void EWebEngine::jsElScrollIntoView(void* ctx, void* el)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || el == nullptr) return;
    litehtml::position p = ((litehtml::element*)el)->get_placement();

    /* Leave a quarter of the viewport above the element so it does not land
     * flush against the top edge. Runs on the ENGINE thread, which owns the
     * document and the authoritative scroll offset (m_engineScroll*); the UI
     * thread is told the new offset via postScrollClamp() once this script run
     * unwinds (m_jsScrollPending -> jsRunPendingNavigation). */
    int y = p.y - self->m_clientHeight / 4;
    if(y < 0) y = 0;
    self->m_engineScrollY = y;

    litehtml::document* doc = self->jsActiveDoc();
    if(doc != nullptr) self->clampScrollLocked(doc->width(), doc->height());
    self->m_jsScrollPending = true;
}

/* ==================================================================
 * Web/BOM bridge callbacks (js_web.h)
 * ================================================================== */

bool EWebEngine::jsWebConfirm(void* ctx, const char* message)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return false;
    EWebUiEvent ev;
    ev.kind = EUET_DIALOG;
    ev.text = std::string("confirm: ") + (message != nullptr ? message : "");
    self->postUiEvent(ev);
    EWEB_LOG("[ewebview] js confirm: %s\n", message != nullptr ? message : "");
    /* Non-interactive. A real confirm() needs a modal answer, and the engine
     * must never block waiting on the UI thread. Answer "cancel": a page that
     * branches on the result then takes the non-destructive path. */
    return false;
}

char* EWebEngine::jsWebPrompt(void* ctx, const char* message, const char* def)
{
    (void)def;
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    EWebUiEvent ev;
    ev.kind = EUET_DIALOG;
    ev.text = std::string("prompt: ") + (message != nullptr ? message : "");
    self->postUiEvent(ev);
    EWEB_LOG("[ewebview] js prompt: %s\n", message != nullptr ? message : "");
    return nullptr;   /* JS null: the user cancelled */
}

void EWebEngine::jsWebGetViewport(void* ctx, int* w, int* h)
{
    EWebEngine* self = (EWebEngine*)ctx;
    /* m_clientWidth/Height are the laid-out viewport, i.e. exactly what
     * innerWidth/innerHeight and matchMedia() must see. */
    if(w) *w = (self != nullptr) ? self->m_clientWidth  : 0;
    if(h) *h = (self != nullptr) ? self->m_clientHeight : 0;
}

void EWebEngine::jsWebGetScreen(void* ctx, int* w, int* h, int* depth)
{
    EWebEngine* self = (EWebEngine*)ctx;
    /* The engine has no display-size query (the viewport is all it knows
     * about), so screen.* reports the viewport. Pages use it for rough layout
     * decisions and for feature detection, both of which the viewport answers
     * correctly on a device where the browser is the whole screen. */
    if(w) *w = (self != nullptr) ? self->m_clientWidth  : 0;
    if(h) *h = (self != nullptr) ? self->m_clientHeight : 0;
    if(depth) *depth = 32;   /* surfaces are ARGB8888 throughout */
}

void EWebEngine::jsWebGetScroll(void* ctx, int* x, int* y)
{
    EWebEngine* self = (EWebEngine*)ctx;
    /* window.scrollX/scrollY report the engine's authoritative scroll offset. */
    if(x) *x = (self != nullptr) ? self->m_engineScrollX : 0;
    if(y) *y = (self != nullptr) ? self->m_engineScrollY : 0;
}

void EWebEngine::jsWebScrollTo(void* ctx, int x, int y)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return;
    self->m_engineScrollX = (x < 0) ? 0 : x;
    self->m_engineScrollY = (y < 0) ? 0 : y;
    /* Runs on the ENGINE thread, which owns the document and the authoritative
     * scroll offset. The UI thread is told the new offset via postScrollClamp()
     * once this script run unwinds (m_jsScrollPending -> jsRunPendingNavigation). */
    litehtml::document* doc = self->jsActiveDoc();
    if(doc != nullptr) self->clampScrollLocked(doc->width(), doc->height());
    self->m_jsScrollPending = true;
}

void EWebEngine::jsWebNavigate(void* ctx, const char* url)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || url == nullptr || url[0] == 0) return;
    /* Resolve against the current document so location.href = "next.html"
     * works; normalizeURL is the same helper loadCSS() uses. */
    std::string full = EWebContainer::normalizeURL(&self->m_port, std::string(url),
                                                   self->m_currentHtmlUrl);
    if(full.empty()) full = url;
    /* Deferred: a navigation tears down the VM this script is running in
     * (cleanupBuildResources), so it cannot happen inline. The engine loop
     * picks the request up once the script unwinds (jsRunPendingNavigation). */
    self->m_jsPendingNav = full;
    EWEB_LOG("[ewebview] js: navigation queued -> %s\n", full.c_str());
}

void EWebEngine::jsWebReload(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return;
    if(self->m_currentHtmlUrl.empty()) {
        /* A page installed via loadHtmlContent() has no URL to re-fetch. */
        EWEB_LOG("[ewebview] js: reload ignored, document has no URL\n");
        return;
    }
    self->m_jsPendingNav = self->m_currentHtmlUrl;
    EWEB_LOG("[ewebview] js: reload queued -> %s\n", self->m_currentHtmlUrl.c_str());
}

/* First value of one response header, case-insensitively matched, or NULL.
 * Same scan EWebContainer.cc applies to its own responses. */
static const char* js_resp_header(const eweb_http_response_t* resp, const char* name)
{
    if(resp == NULL || resp->headers == NULL) return NULL;
    size_t nlen = strlen(name);
    for(int i = 0; i < resp->header_count; i++) {
        const char* k = resp->headers[i].key;
        if(k == NULL) continue;
        if(strncasecmp(k, name, nlen) == 0 && (k[nlen] == 0 || k[nlen] == ':'))
            return resp->headers[i].value;
    }
    return NULL;
}

bool EWebEngine::jsWebRequest(void* ctx, const char* method, const char* url,
                              const char* headers, const char* body,
                              int* status, char** out_body, char** out_headers)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || url == nullptr || url[0] == 0) return false;
    if(status != nullptr) *status = 0;
    if(out_body != nullptr) *out_body = nullptr;
    if(out_headers != nullptr) *out_headers = nullptr;
    const eweb_port_t* port = &self->m_port;
    if(!port->net.request || !port->net.free_response) return false;

    /* Relative refs resolve against the document the script is running in,
     * exactly like <script src> does. */
    std::string full = EWebContainer::getFullURL(port, url, self->jsDocumentUrl());
    if(full.empty()) return false;

    /* Split the "\r\n"-joined request header block into (key,value) pairs.
     * keys/vals keep stable copies alive; hdrs points into them and both
     * outlive every request below. */
    std::vector<std::string> keys, vals;
    std::vector<eweb_http_header_t> hdrs;
    if(headers != nullptr) {
        const char* p = headers;
        while(*p != 0) {
            const char* eol = strstr(p, "\r\n");
            size_t len = eol ? (size_t)(eol - p) : strlen(p);
            const char* colon = (const char*)memchr(p, ':', len);
            if(colon != nullptr && colon > p) {
                keys.push_back(std::string(p, colon - p));
                const char* v = colon + 1;
                size_t vlen = len - (size_t)(v - p);
                while(vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
                vals.push_back(std::string(v, vlen));
            }
            p += len;
            if(eol != nullptr) p += 2;
        }
        for(size_t i = 0; i < keys.size(); i++) {
            eweb_http_header_t h;
            h.key = keys[i].c_str();
            h.value = vals[i].c_str();
            hdrs.push_back(h);
        }
    }

    int body_len = (body != nullptr) ? (int)strlen(body) : 0;
    const char* mth = (method != nullptr && method[0] != 0) ? method : "GET";

    /* The port never follows redirects, so hop here - capped, resolving a
     * relative Location against the URL it came from, and re-scoping the
     * cookie jar per hop like the resource path does. A 303 (or a 301/302
     * answering a POST) comes back as a plain GET, body dropped. */
    std::string cur = full;
    std::string cur_method = mth;
    const char* cur_body = body;
    int cur_body_len = body_len;
    eweb_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    bool got = false;
    for(int hop = 0; hop < 5; hop++) {
        /* Session cookies for THIS hop: an API that logged the page in with
         * Set-Cookie keeps working across its fetches. Same-site to the
         * document, never top-level. */
        std::string cookie = EWebCookieJar::instance().requestHeader(
                cur, self->jsDocumentUrl(), false);
        std::vector<eweb_http_header_t> send = hdrs;
        std::string cookie_storage;   /* keeps cookie.c_str() alive per hop */
        if(!cookie.empty()) {
            cookie_storage = cookie;
            eweb_http_header_t ch;
            ch.key = "Cookie";
            ch.value = cookie_storage.c_str();
            send.push_back(ch);
        }

        if(!port->net.request(port->net.ud, cur.c_str(), cur_method.c_str(),
                              cur_body, cur_body_len,
                              send.empty() ? NULL : &send[0], (int)send.size(),
                              &resp)) {
            return false;   /* transport failure => XHR status 0 / rejected fetch */
        }
        if(resp.error) {
            port->net.free_response(port->net.ud, &resp);
            return false;
        }

        std::vector<std::string> set_cookies;
        for(int i = 0; i < resp.header_count; i++) {
            const char* k = resp.headers[i].key;
            if(k != NULL && strcasecmp(k, "set-cookie") == 0 && resp.headers[i].value != NULL)
                set_cookies.push_back(std::string(resp.headers[i].value));
        }
        if(!set_cookies.empty())
            EWebCookieJar::instance().storeResponseCookies(cur, set_cookies);

        const char* location = js_resp_header(&resp, "location");
        bool redir = (resp.status == 301 || resp.status == 302 || resp.status == 303 ||
                      resp.status == 307 || resp.status == 308) && location != NULL;
        if(!redir) { got = true; break; }

        std::string next = EWebContainer::getFullURL(port, location, cur);
        int prev_status = resp.status;
        port->net.free_response(port->net.ud, &resp);
        memset(&resp, 0, sizeof(resp));
        if(next.empty() || next == cur) return false;   /* redirect loop / junk */
        if(prev_status == 303 ||
           ((prev_status == 301 || prev_status == 302) && cur_method != "GET")) {
            cur_method = "GET";
            cur_body = NULL;
            cur_body_len = 0;
        }
        cur = next;
    }
    if(!got) return false;

    if(status != nullptr) *status = resp.status;

    /* The bridge adopts both outputs and frees them with mario_free, so they
     * are mario-owned NUL-terminated copies (a binary body survives as bytes
     * plus the terminator; js_web.c treats it as a string anyway). */
    if(out_body != nullptr) {
        int n = resp.body_size > 0 ? resp.body_size : 0;
        char* ob = (char*)mario_malloc((uint32_t)n + 1);
        if(ob == nullptr) { port->net.free_response(port->net.ud, &resp); return false; }
        if(n > 0) memcpy(ob, resp.body, (size_t)n);
        ob[n] = 0;
        *out_body = ob;
    }
    if(out_headers != nullptr) {
        std::string block;
        for(int i = 0; i < resp.header_count; i++) {
            if(resp.headers[i].key == NULL || resp.headers[i].value == NULL) continue;
            if(!block.empty()) block += "\r\n";
            block += resp.headers[i].key;
            block += ": ";
            block += resp.headers[i].value;
        }
        *out_headers = js_strdup_mario(block.c_str());
    }
    port->net.free_response(port->net.ud, &resp);
    return true;
}

char* EWebEngine::jsWebGetCookie(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    /* The jar itself applies domain/path/Secure/SameSite matching against this
     * document's URL and filters HttpOnly entries out, so what comes back is
     * already the "k=v; k2=v2" string document.cookie reports. A document with
     * no URL (loadHtmlContent) gets an empty jar view, as an about:blank does. */
    return js_strdup_mario(EWebCookieJar::instance().jsGet(self->jsDocumentUrl()).c_str());
}

void EWebEngine::jsWebSetCookie(void* ctx, const char* cookie)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return;
    /* js_web.c hands over the whole Set-Cookie-shaped string, so Path/Domain/
     * Max-Age/Expires/SameSite are honoured here; HttpOnly is dropped (only a
     * server may set it). Stored against the document's own site, which is
     * what makes it reach the HTTP side too. */
    EWebCookieJar::instance().jsSet(self->jsDocumentUrl(),
                                    cookie != nullptr ? cookie : "");
}

char* EWebEngine::jsWebStorageLoad(void* ctx, bool session)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    const std::string& blob = session ? self->m_jsSessionStorage : self->m_jsLocalStorage;
    if(blob.empty()) return nullptr;   /* nothing stored yet */
    return js_strdup_mario(blob.c_str());
}

void EWebEngine::jsWebStorageSave(void* ctx, bool session, const char* blob)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return;
    std::string& slot = session ? self->m_jsSessionStorage : self->m_jsLocalStorage;
    slot = (blob != nullptr) ? blob : "";
}

/* ==================================================================
 * Bridge registration
 * ================================================================== */

void EWebEngine::registerEventNatives(struct st_vm* vm)
{
    if(vm == nullptr) return;
    /* js_event.c takes no callback table of its own: it reaches the embedder
     * through the DOM bridge's context and callbacks (js_dom_ctx /
     * js_dom_callbacks), so `this` and the js_dom_callbacks_t filled in by
     * initJsVm() are already in place. All it needs is for the Document and
     * Element classes and the window/document globals to exist, which
     * js_register_dom_natives() guarantees. */
    if(!js_register_event_natives(vm)) {
        EWEB_LOG("[ewebview] js: event native registration failed\n");
    }
}

void EWebEngine::registerWebNatives(struct st_vm* vm)
{
    if(vm == nullptr) return;

    js_web_callbacks_t cb;
    js_web_callbacks_init(&cb);

    /* Modal dialogs. Non-interactive (see the callbacks above): they post
     * EUET_DIALOG, which the embedder surfaces exactly like alert(). */
    cb.confirm      = jsWebConfirm;
    cb.prompt       = jsWebPrompt;

    /* Viewport / screen / scroll, straight out of the engine's own fields. */
    cb.get_viewport = jsWebGetViewport;
    cb.get_screen   = jsWebGetScreen;
    cb.get_scroll   = jsWebGetScroll;
    cb.scroll_to    = jsWebScrollTo;

    /* Navigation. Both defer through m_jsPendingNav because a navigation tears
     * down the VM the requesting script is running in.
     *
     * history_back / history_forward / history_length stay NULL on purpose:
     * this engine owns no session history - the embedding app owns the
     * back/forward controls and the URL list. js_web.c then reports
     * history.length as 1 (clamped to at least one entry, matching a browser
     * looking at a fresh page) and makes back()/forward()/go() no-ops, while
     * pushState()/replaceState() still work because the bridge keeps the state
     * object itself.
     *
     * navigator identity (get_user_agent / get_language / get_platform) also
     * stays NULL: js_web.c's built-in defaults already name the embedder and
     * are the same for every engine, so there is nothing per-instance to
     * report. */
    cb.navigate     = jsWebNavigate;
    cb.reload       = jsWebReload;

    /* document.cookie goes to the process-wide EWebCookieJar, scoped to this
     * document's URL: get_cookie sees the non-HttpOnly entries matching it,
     * set_cookie stores against it with all attributes honoured. The jar is
     * the same one EWebContainer::loadURL reads and writes, so a cookie a
     * script sets is sent on that site's next request and a Set-Cookie a
     * server sent is readable from the page - and nothing leaks to another
     * site. */
    cb.get_cookie   = jsWebGetCookie;
    cb.set_cookie   = jsWebSetCookie;

    /* Web Storage, same lifetime story: the blobs live on the engine, so
     * localStorage persists across page loads within one browser session.
     * Nothing reaches the disk yet, so it does not survive the app. */
    cb.storage_load = jsWebStorageLoad;
    cb.storage_save = jsWebStorageSave;

    /* XHR/fetch go through jsWebRequest, which performs the request right on
     * the engine thread via port->net.request instead of deferring through the
     * download task queue. The hook contract demands a blocking call, and the
     * VM runs on the engine thread itself, so routing it through the queue
     * (whose results only that same thread drains) would deadlock it - a
     * direct synchronous port call cannot. The engine loop simply stalls for
     * the duration of one request (the port's 10s timeout caps it), which is
     * exactly what a synchronous XHR does to a browser's page thread too.
     * With the hook wired, XMLHttpRequest/fetch/Response/Headers are live;
     * a transport failure still reports XHR readyState 4 / status 0 and a
     * rejected fetch Promise, like a browser does for a failed request. */
    cb.http_request = jsWebRequest;

    if(!js_register_web_natives(vm, &cb)) {
        EWEB_LOG("[ewebview] js: web native registration failed\n");
    }
}

/* ==================================================================
 * Lifecycle / input wiring
 * ================================================================== */

void EWebEngine::jsInvalidateHandles()
{
    if(m_jsVm != nullptr) {
        /* Listeners are keyed by element handle and the DOM bridge caches one
         * JS wrapper per handle; both go stale the moment the document they
         * point into is rebuilt. Dropping them here is what makes a
         * document.write() reparse safe - the rebuilt page re-registers its
         * own listeners when its scripts run again. */
        js_event_clear_listeners(m_jsVm);
        js_dom_reset_element_cache(m_jsVm);
    }
    m_jsHoverElement = nullptr;
}

void EWebEngine::jsFreeDetachedNodes()
{
    for(size_t i = 0; i < m_jsDetached.size(); ++i) {
        /* Placement-new'd over malloc (see jsCreateTextNode), so the default
         * operator delete resolves to free() - the same release path
         * html_tag's destructor uses for its children. */
        delete m_jsDetached[i];
    }
    if(!m_jsDetached.empty()) {
        EWEB_LOG("[ewebview] js: freed %d detached node(s)\n", (int)m_jsDetached.size());
    }
    m_jsDetached.clear();
    m_jsHoverElement = nullptr;
}

void EWebEngine::jsFireLoadEvents()
{
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled || m_jsInScript) return;
    /* DOMContentLoaded first, then load - the order every page assumes. The
     * event bridge runs <body onload> as part of fire_load, since HTML treats
     * that attribute as the window's load slot. Called once BUILD_SWAP_DOC has
     * installed and laid out the page, so a handler that reads
     * getBoundingClientRect() or offsetHeight sees real numbers. Engine-thread
     * only: the VM is entered here alone, so no lock is needed. m_jsInScript
     * arms the VM interrupt hook so a heavy onload handler stays interruptible
     * by a termination request (m_buildAbort). */
    jsVmEnter();
    js_event_fire_dom_content_loaded(m_jsVm);
    js_event_fire_load(m_jsVm);
    jsVmExit();
}

void EWebEngine::jsFireResizeEvent()
{
    /* Engine-thread only (ECMD_RESIZE handler): m_jsInScript means we were
     * re-entered from inside a VM run, where firing another event would
     * corrupt it - the DOM resize event is skipped; the engine-level resize
     * (re-render, scroll clamp) has already happened in engineHandleCommand. */
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled || m_jsInScript) return;
    jsVmEnter();
    js_event_fire_resize(m_jsVm, m_clientWidth, m_clientHeight);
    jsVmExit();
}

void EWebEngine::jsFireScrollEvent()
{
    /* Engine-thread only (ECMD_SCROLL handler / jsRunPendingNavigation). */
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled || m_jsInScript) return;
    jsVmEnter();
    js_event_fire_scroll(m_jsVm);
    jsVmExit();
}

void EWebEngine::jsRunPendingNavigation()
{
    /* Engine-thread only: run by the engine loop after a script unwinds. Both
     * deferred requests are engine-owned state, so no locking is needed. */
    if(m_jsScrollPending) {
        m_jsScrollPending = false;
        /* The engine scroll offset already moved (jsWebScrollTo); publish it so
         * the UI snaps its live offset and refreshes the scrollbar. The engine
         * loop re-renders the exposed strip on its own (scroll != cacheScroll),
         * so no explicit invalidate is needed here. */
        postScrollClamp();
        jsFireScrollEvent();
    }
    if(!m_jsPendingNav.empty()) {
        std::string url = m_jsPendingNav;
        m_jsPendingNav.clear();
        /* engineNavigate() tears the VM down (cleanupBuildResources) and queues
         * the fetch, so this must be the last thing done with JS state. */
        engineNavigate(url);
    }
}

bool EWebEngine::jsDispatchMouseEvent(int mouseState, int button, int cx, int cy)
{
    if(m_jsVm == nullptr || !m_jsEnabled) return true;
    /* Engine-thread only (ECMD_INPUT handler): m_jsInScript means a VM run is
     * in flight on this thread and must not be re-entered - no DOM mouse
     * events fire and the default action proceeds. */
    if(m_jsInScript) return true;

    const char* type = nullptr;
    int click_count = 1;
    bool track_hover = false;
    switch(mouseState) {
        case EWEB_MOUSE_DOWN:         type = "mousedown"; break;
        case EWEB_MOUSE_UP:           type = "mouseup";   break;
        case EWEB_MOUSE_CLICK:        type = "click";     break;
        case EWEB_MOUSE_DOUBLE_CLICK: type = "dblclick";  click_count = 2; break;
        case EWEB_MOUSE_MOVE:         type = "mousemove"; track_hover = true; break;
        default:
            /* EWEB_MOUSE_WHEEL and anything else maps to no DOM event: the
             * embedder turns a wheel gesture into ewebview_scroll(). */
            return true;
    }

    /* cx/cy are the viewport-relative client coords the embedder pre-computed
     * and forwarded in the ECMD_INPUT command. */
    int domButton = 0;
    if(button == EWEB_BUTTON_RIGHT)       domButton = 2;
    else if(button == EWEB_BUTTON_MIDDLE) domButton = 1;

    bool allowed = true;

    litehtml::document* doc = jsActiveDoc();
    litehtml::element* target = nullptr;
    if(doc != nullptr) {
        litehtml::element::ptr root = doc->root();
        if(root != nullptr) {
            /* The hit tester works in unscrolled document coordinates, so add
             * the engine's scroll offsets; the client-relative pair stays as-is
             * because that is what position:fixed elements are tested against. */
            target = root->get_element_by_point(cx + m_engineScrollX, cy + m_engineScrollY, cx, cy);
        }
        if(target == nullptr && root != nullptr) target = root;   /* blank area: the root element */
    }

    if(target != nullptr) {
        /* m_jsInScript arms the VM interrupt hook for the handlers' duration:
         * a heavy onclick/onmousemove listener stays interruptible by a
         * termination request (m_buildAbort) instead of pinning the engine. */
        jsVmEnter();
        if(track_hover && target != (litehtml::element*)m_jsHoverElement) {
            litehtml::element* prev = (litehtml::element*)m_jsHoverElement;
            m_jsHoverElement = (void*)target;
            if(prev != nullptr) {
                js_event_dispatch_mouse(m_jsVm, (void*)prev, "mouseout",
                                        cx, cy, m_engineScrollX, m_engineScrollY, domButton, 0);
            }
            js_event_dispatch_mouse(m_jsVm, (void*)target, "mouseover",
                                    cx, cy, m_engineScrollX, m_engineScrollY, domButton, 0);
        }
        allowed = js_event_dispatch_mouse(m_jsVm, (void*)target, type,
                                          cx, cy, m_engineScrollX, m_engineScrollY,
                                          domButton, click_count);
        jsVmExit();
    }

    /* A listener that mutated the DOM already called jsMarkLayoutDirty(), and
     * the engine loop's applyPendingLayoutUpdates() turns that into a re-layout
     * and a fresh frame, so there is nothing to invalidate here. `allowed` is
     * false when a handler called preventDefault(); the caller (ECMD_INPUT)
     * uses it to skip the default action (anchor following). */
    return allowed;
}

void EWebEngine::jsDispatchSimpleEvent(litehtml::element* el, const char* type, bool bubbles)
{
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled || m_jsInScript) return;
    if(el == nullptr || type == nullptr) return;
    jsVmEnter();
    js_event_dispatch_simple(m_jsVm, (void*)el, type, bubbles);
    jsVmExit();
}

bool EWebEngine::jsDispatchCancelableEvent(litehtml::element* el, const char* type, bool bubbles)
{
    /* Like jsDispatchSimpleEvent but cancelable, so a page handler can call
     * preventDefault() (e.g. a "submit" listener doing an XHR submit instead of
     * a navigation). Returns false when prevented, true otherwise - including
     * when there is no VM, so the caller's default action still runs. */
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled || m_jsInScript) return true;
    if(el == nullptr || type == nullptr) return true;
    js_event_init_t in;
    js_event_init(&in, type);
    in.on = JS_EVENT_ON_ELEMENT;
    in.target = (void*)el;
    in.bubbles = bubbles;
    in.cancelable = true;
    in.trusted = true;
    jsVmEnter();
    bool allowed = js_event_dispatch(m_jsVm, &in);
    jsVmExit();
    return allowed;
}

/* ==================================================================
 * Keyboard input -> DOM key events
 * ================================================================== */

/* Map an EWEB_KEY_* code (plus the CHAR payload) onto the DOM KeyboardEvent
 * `key` string and the legacy `keyCode`. Letters and digits reach the engine
 * as EWEB_KEY_CHAR with their UTF-8 bytes in `text`, which is what DOM reports
 * for a printable key; the non-printable codes get their standard names. */
static void ewebKeyToDom(int key, const char* text,
                         char* out, size_t outsz, int* outCode)
{
    out[0] = 0;
    *outCode = 0;
    switch(key) {
    case EWEB_KEY_CHAR:
        if(text != nullptr && text[0] != 0) snprintf(out, outsz, "%s", text);
        /* Legacy keyCode for a printable char is the upper-case ASCII code
         * point when it is a single ASCII byte; multi-byte UTF-8 leaves 0. */
        if(text != nullptr && text[0] != 0 && text[1] == 0 &&
                (unsigned char)text[0] < 0x80) {
            char c = text[0];
            *outCode = (c >= 'a' && c <= 'z') ? (int)(c - 'a' + 'A') : (int)(unsigned char)c;
        }
        return;
    case EWEB_KEY_BACKSPACE: snprintf(out, outsz, "Backspace");  *outCode = 8;  return;
    case EWEB_KEY_TAB:       snprintf(out, outsz, "Tab");        *outCode = 9;  return;
    case EWEB_KEY_ENTER:     snprintf(out, outsz, "Enter");      *outCode = 13; return;
    case EWEB_KEY_ESCAPE:    snprintf(out, outsz, "Escape");     *outCode = 27; return;
    case EWEB_KEY_SPACE:     snprintf(out, outsz, " ");          *outCode = 32; return;
    case EWEB_KEY_DELETE:    snprintf(out, outsz, "Delete");     *outCode = 46; return;
    case EWEB_KEY_LEFT:      snprintf(out, outsz, "ArrowLeft");  *outCode = 37; return;
    case EWEB_KEY_UP:        snprintf(out, outsz, "ArrowUp");    *outCode = 38; return;
    case EWEB_KEY_RIGHT:     snprintf(out, outsz, "ArrowRight"); *outCode = 39; return;
    case EWEB_KEY_DOWN:      snprintf(out, outsz, "ArrowDown");  *outCode = 40; return;
    case EWEB_KEY_HOME:      snprintf(out, outsz, "Home");       *outCode = 36; return;
    case EWEB_KEY_END:       snprintf(out, outsz, "End");        *outCode = 35; return;
    case EWEB_KEY_PAGEUP:    snprintf(out, outsz, "PageUp");     *outCode = 33; return;
    case EWEB_KEY_PAGEDOWN:  snprintf(out, outsz, "PageDown");   *outCode = 34; return;
    case EWEB_KEY_INSERT:    snprintf(out, outsz, "Insert");     *outCode = 45; return;
    default: break;
    }
    /* F1..F12 are contiguous from EWEB_KEY_F1. */
    if(key >= EWEB_KEY_F1 && key <= EWEB_KEY_F12) {
        snprintf(out, outsz, "F%d", key - EWEB_KEY_F1 + 1);
        *outCode = 112 + (key - EWEB_KEY_F1);
        return;
    }
    /* A bare letter/digit code (ASCII) forwarded without a CHAR payload: DOM
     * `key` is the character itself, upper-cased when Shift is not modelled
     * here (the embedder already folded shift into the CHAR text). */
    if(key > 0 && key < 0x1000) {
        out[0] = (char)key;
        out[1] = 0;
        *outCode = (key >= 'a' && key <= 'z') ? (int)(key - 'a' + 'A') : (int)key;
    }
}

/* Translate the EWEB_MOD_* bitmask into the JS_EVENT_MOD_* one the event
 * bridge reads for KeyboardEvent.altKey/ctrlKey/shiftKey/metaKey. */
static unsigned ewebModsToDom(int mods)
{
    unsigned m = 0;
    if(mods & EWEB_MOD_SHIFT) m |= JS_EVENT_MOD_SHIFT;
    if(mods & EWEB_MOD_CTRL)  m |= JS_EVENT_MOD_CTRL;
    if(mods & EWEB_MOD_ALT)   m |= JS_EVENT_MOD_ALT;
    if(mods & EWEB_MOD_META)  m |= JS_EVENT_MOD_META;
    return m;
}

void EWebEngine::handleKeyEvent(const eweb_key_event_t& kev)
{
    /* ENGINE-THREAD ONLY (ECMD_KEY handler). Dispatch the DOM key events to
     * the focused element (or the document when nothing is focused) and then
     * run the engine's default action unless a handler called preventDefault().
     *
     * The VM is optional: with no VM (or JS disabled) the events simply do not
     * fire, but the default action (editing / activation / focus traversal,
     * wired up in the later stages) still runs so a JS-free page is fully
     * usable. A CHAR keydown also carries a keypress, matching the DOM order
     * keydown -> keypress -> (text inserted) -> keyup.
     *
     * KEY UP only produces a keyup event; the default action hangs off keydown
     * (and the CHAR text insertion off keypress), exactly as a browser does. */
    if(kev.type == EWEB_KEYSTATE_UP) {
        if(m_jsVm != nullptr && m_jsEnabled && !m_jsPageDisabled && !m_jsInScript) {
            char key[16];
            int code = 0;
            ewebKeyToDom(kev.key, kev.text, key, sizeof(key), &code);
            jsVmEnter();
            js_event_dispatch_key(m_jsVm, m_focusElement, "keyup",
                                  key, code, ewebModsToDom(kev.mods));
            jsVmExit();
        }
        return;
    }

    /* EWEB_KEYSTATE_DOWN (and the CHAR text-insertion path). */
    char key[16];
    int code = 0;
    ewebKeyToDom(kev.key, kev.text, key, sizeof(key), &code);
    unsigned mods = ewebModsToDom(kev.mods);

    bool allowed = true;
    if(m_jsVm != nullptr && m_jsEnabled && !m_jsPageDisabled && !m_jsInScript) {
        jsVmEnter();
        allowed = js_event_dispatch_key(m_jsVm, m_focusElement, "keydown",
                                        key, code, mods);
        /* keypress fires only for a character-producing key, and only when the
         * keydown was not cancelled. */
        if(allowed && kev.key == EWEB_KEY_CHAR && kev.text[0] != 0) {
            allowed = js_event_dispatch_key(m_jsVm, m_focusElement, "keypress",
                                            kev.text, code, mods);
        }
        jsVmExit();
    }

    if(!allowed)
        return;   /* a handler called preventDefault(): no default action */

    /* Default action. The editing / activation / focus-traversal behaviour is
     * layered in by the later implementation stages; this is the single hook
     * point they extend. */
    handleKeyDefault(kev, key, mods);
}

/* ==================================================================
 * DOM-mutation journal (survives a document.write() reparse)
 * ================================================================== */

void EWebEngine::recordJsMutation(int kind, const std::string& id,
                                  const std::string& name, const std::string& value)
{
    /* Keep only the newest value per (kind, id, name): replaying an earlier
     * value that a later script overwrote would resurrect stale state. */
    for(auto& m : m_jsMutations) {
        if(m.kind == kind && m.id == id && m.name == name) {
            m.value = value;
            return;
        }
    }
    JsMutation m;
    m.kind = kind;
    m.id = id;
    m.name = name;
    m.value = value;
    m_jsMutations.push_back(m);
}

void EWebEngine::replayJsMutations()
{
    if(m_jsMutations.empty() || m_buildDoc == nullptr) return;
    int applied = 0;
    for(const auto& m : m_jsMutations) {
        if(m.kind == 2) {
            litehtml::element::ptr root = m_buildDoc->root();
            if(root == nullptr) continue;
            litehtml::element::ptr t = root->select_one("title");
            if(t == nullptr) continue;
            js_set_element_text(t, m.value.c_str(), &m_jsDetached);
            applied++;
            continue;
        }
        litehtml::element* el = (litehtml::element*)jsGetElementById(this, m.id.c_str());
        if(el == nullptr) continue;
        if(m.kind == 0) {
            js_set_element_text(el, m.value.c_str(), &m_jsDetached);
        } else if(m.kind == 1) {
            el->set_attr(m.name.c_str(), m.value.c_str());
        } else if(m.kind == 3) {
            js_apply_inner_html(el, m.value, &m_jsDetached);   /* innerHTML: re-parse */
        }
        applied++;
    }
    if(applied > 0) {
        EWEB_LOG("[ewebview] js: replayed %d mutation(s) after reparse\n", applied);
        markLayoutDirty(true);
    }
}

} /* namespace eweb */
