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
#include <ctype.h>
#include <new>

/* mario entry points that live in libmario.a but are not declared in a public
 * header: the JS bytecode compiler (passed to vm_new) and the all-natives
 * registrar (passed to vm_init so console/Object/Array/... exist). */
extern "C" bool js_compile(bytecode_t* bc, const char* input);
extern "C" void reg_all_natives(vm_t* vm);

namespace eweb {

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
    cb.el_remove_attr    = jsElRemoveAttr;
    cb.el_get_rect       = jsElGetRect;
    cb.el_get_style      = jsElGetStyle;
    cb.el_focus          = jsElFocus;
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

void EWebEngine::runPageScripts()
{
    /* A page whose only JavaScript lives in inline on* attributes (onload,
     * onclick, ...) still needs a VM: those handlers are compiled on demand by
     * the event bridge and resolve against the globals it installs, so bailing
     * out on an empty script list would leave them uncallable. A page with
     * neither is skipped outright - vm_init plus four bridge registrations is
     * too much memory to spend on plain markup. */
    if(!m_jsEnabled || m_jsPageDisabled || m_buildDoc == nullptr) return;
    if(m_jsVm == nullptr && m_jsScripts.empty() && !m_jsHasInlineHandlers) return;
    initJsVm();
    if(m_jsVm == nullptr) return;

    for(size_t i = 0; i < m_jsScripts.size(); ++i) {
        const std::string& src = m_jsScripts[i];
        if(src.empty()) continue;
        /* vm_load_run appends this script's bytecode after the previous one and
         * runs it; globals persist in vm->root across scripts, matching
         * separate <script> blocks that share one global scope. m_jsInScript
         * arms the VM step hook, which enforces the run budget and honours a
         * termination abort; window interaction stays live on its own thread
         * even through this pre-paint (document.write) script run. */
        jsVmEnter();
        if(!vm_load_run(m_jsVm, src.c_str())) {
            EWEB_LOG("[ewebview] js: script %d failed to compile\n", (int)i);
        }
        jsVmExit();
    }
    EWEB_LOG("[ewebview] js: ran %d script(s)\n", (int)m_jsScripts.size());
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
        size_t i = m_jsNextScript++;
        const std::string& src = m_jsScripts[i];
        if(src.empty()) continue;
        uint64_t run_start = ticMs();
        /* Bracket vm_load_run so the DOM-bridge mutation callbacks push the
         * page to the screen mid-script (jsMarkLayoutDirty -> jsProgressiveFlush
         * -> engineRenderFrame): a long test script then shows its results as
         * they are produced. m_jsInScript arms the step hook (run budget +
         * abort). Window interaction is unaffected - it runs on the UI thread. */
        m_jsProgressiveActive = true;
        jsVmEnter();
        m_jsLastFlushAt = run_start;
        if(!vm_load_run(m_jsVm, src.c_str())) {
            EWEB_LOG("[ewebview] js: script %d failed to compile\n", (int)i);
        }
        jsVmExit();
        m_jsProgressiveActive = false;
        EWEB_LOG("[ewebview] js: script %d ran %u ms (post-swap)\n",
            (int)i, (uint32_t)(ticMs() - run_start));
        /* Paint what this script produced before the next one runs. */
        jsProgressiveFlush(true);
        break;
    }
    return m_jsNextScript < m_jsScripts.size();
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
    m_jsEnterGen = m_buildAbortGen;
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
    /* An abort raised by a termination/navigation request DURING this run (the
     * generation moved past the jsVmEnter snapshot) is intentional and must NOT
     * count against the page's watchdog budget; only a genuine run-budget
     * timeout does. A stale flag (STOP consumed while this page sat idle) has
     * the same generation as the snapshot, so runs after it count normally. */
    if(m_buildAbortGen == m_jsEnterGen) {
        m_jsAbortCount++;
        EWEB_LOG("[ewebview] js: script run aborted by watchdog (%d/%d)\n",
             m_jsAbortCount, kJsRunAbortMax);
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
    if(m_jsEnterAt != 0 && (now - m_jsEnterAt) > kJsRunBudgetMs) {
        EWEB_LOG("[ewebview] js: run budget %u ms exceeded - terminating\n",
             (unsigned)kJsRunBudgetMs);
        vm->terminated = true;   /* unwind every nested vm_run frame */
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
    /* MVP: no fragment parser -> strip tags and set as text. */
    std::string stripped = js_strip_tags(html != nullptr ? html : "");
    js_set_element_text((litehtml::element*)el, stripped.c_str(),
                        self != nullptr ? &self->m_jsDetached : nullptr);
    if(self != nullptr) {
        const char* idv = ((litehtml::element*)el)->get_attr("id", nullptr);
        if(idv != nullptr && idv[0] != 0)
            self->recordJsMutation(0, idv, "", stripped);
        self->jsMarkLayoutDirty();
    }
}

char* EWebEngine::jsElGetAttr(void* ctx, void* el, const char* name)
{
    (void)ctx;
    if(el == nullptr || name == nullptr) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    const char* v = e->get_attr(name, nullptr);
    if(v == nullptr) return nullptr;
    return js_strdup_mario(v);
}

void EWebEngine::jsElSetAttr(void* ctx, void* el, const char* name, const char* value)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr || name == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    e->set_attr(name, value != nullptr ? value : "");
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
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    return (void*)root->select_one("body");
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

void* EWebEngine::jsElParent(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return nullptr;
    return (void*)((litehtml::element*)el)->parent();
}

int EWebEngine::jsElChildCount(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return 0;
    return (int)((litehtml::element*)el)->get_children_count();
}

void* EWebEngine::jsElChild(void* ctx, void* el, int idx)
{
    (void)ctx;
    if(el == nullptr || idx < 0) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    if((size_t)idx >= e->get_children_count()) return nullptr;
    return (void*)e->get_child(idx);
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
    return (t != nullptr && t[0] != 0);
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
    /* html_tag::remove_attr drops the attribute outright, so a later
     * getAttribute reports null rather than "" - which set_attr(name, "")
     * cannot express. It also clears the style-property cache and the parsed
     * class list, so removing class= really stops matching .foo selectors. */
    ((litehtml::element*)el)->remove_attr(name);
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
    (void)ctx; (void)el;
    /* litehtml has no focus model wired into this engine - el_input only draws
     * the field (get_content_size / render / draw; it keeps no text state), so
     * there is no caret to move - and keyboard focus belongs to the embedder's
     * window, not to an element. Accept the call and do nothing: pages call
     * focus() defensively and must not throw. */
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

    /* http_request stays NULL. It MUST block the VM thread until the response
     * is complete, but this engine's HTTP path is an asynchronous task queue
     * (download worker -> m_resultQueue -> processResults() on the engine
     * loop). The VM runs on that same engine thread, so waiting for a download
     * would deadlock the very loop that delivers it.
     *
     * With the hook absent js_web.c still installs XMLHttpRequest, fetch(),
     * Response and Headers; every request reports a network error (XHR
     * readyState 4 / status 0, fetch a rejected Promise) instead of throwing,
     * which is what a browser does for a failed request. */

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
        }
        applied++;
    }
    if(applied > 0) {
        EWEB_LOG("[ewebview] js: replayed %d mutation(s) after reparse\n", applied);
        markLayoutDirty(true);
    }
}

} /* namespace eweb */
