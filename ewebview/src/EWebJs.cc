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
#include <stdlib.h>    /* getenv (env-gated net diag) */
#include "eweb_el_input.h"

#include <mario/mario.h>
#include <mario/js_dom.h>
#include <mario/js_event.h>
#include <mario/js_web.h>
#include <mario/js_wasm.h>

/* mario's bcdump.h is not exported into the installed include tree; declare the
 * one diagnostic entry point we need (EWEB_SPINDBG cut-point disassembly). */
extern "C" void bc_dump_window(bytecode_t* bc, PC center, PC radius);

/* el_text is not pulled in by <litehtml.h>; createTextNode and the text
 * setters build one directly (malloc + placement-new, matching litehtml_alloc). */
#include <litehtml/el_text.h>
#include <litehtml/el_comment.h>

#include <string>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <new>
#include <setjmp.h>   /* SEGV recovery checkpoints (see eweb_segv_checkpoint) */
#include <pthread.h>  /* ES-module prefetch workers (see ModulePrefetch) */
#include <deque>
#include <map>
#include <vector>
#include <signal.h>
#include <unistd.h>   /* usleep (await spin tick idle wait) */

/* mario entry points that live in libmario.a but are not declared in a public
 * header: the JS bytecode compiler (passed to vm_new) and the all-natives
 * registrar (passed to vm_init so console/Object/Array/... exist). */
extern "C" bool js_compile(bytecode_t* bc, const char* input);
extern "C" void reg_all_natives(vm_t* vm);
/* Promise debug ledger (native_Promise.h is not on the ewebview include path). */
extern "C" void mario_promise_ledger_dump(vm_t* vm);

/* ==================================================================
 * SIGSEGV recovery checkpoints
 *
 * The mario VM is fed large minified third-party bundles; a latent memory
 * fault inside one script or one timer callback must not take down the whole
 * browser. Each run boundary (a page script, the timer poll) arms a sigsetjmp
 * checkpoint; the port's SIGSEGV handler calls eweb_segv_try_recover(), which
 * siglongjmps back to the innermost armed checkpoint. The caller then resets
 * the VM and skips the faulting unit instead of aborting the process.
 * ================================================================== */
#ifdef __ewokos__
/* EwokOS libc has no sigjmp_buf/sigsetjmp; and plain setjmp/longjmp must not
 * be called by name from compiled code - GCC intercepts those identifiers and
 * emits __builtin_setjmp/__builtin_longjmp, whose minimal buffer layout
 * disagrees with the libc assembly implementations. libewoksys ships the same
 * save/restore code under the gumbo_* names (setjmp_<arch>.S), which GCC
 * leaves alone. The signal-mask save the sig* variants would do is a no-op on
 * EwokOS anyway: the kernel only delivers STOP/KILL. */
extern "C" int  gumbo_setjmp(jmp_buf env);
extern "C" void gumbo_longjmp(jmp_buf env, int val);
#define eweb_jmp_buf         jmp_buf
#define eweb_setjmp(env)     gumbo_setjmp(env)
#define eweb_longjmp(env, v) gumbo_longjmp((env), (v))
#else
#define eweb_jmp_buf         sigjmp_buf
#define eweb_setjmp(env)     sigsetjmp((env), 1)
#define eweb_longjmp(env, v) siglongjmp((env), (v))
#endif

static eweb_jmp_buf             s_segv_jmp;
static volatile sig_atomic_t    s_segv_armed = 0;
/* TEMP DIAG (ghfix6): crash#1 recovers with NO checkpoint-branch output and NO
 * vm_reset_pending, which no live checkpoint can do - so the longjmp may land
 * in a DEAD frame (armed left set after an arm..disarm window was unwound by a
 * non-SIGSEGV path). Tag each arm site with an id and its frame pointer; on
 * recover, compare: the fault must sit DEEPER than the checkpoint's frame or
 * the checkpoint frame is gone and the jump is UB. */
static int                      s_segv_id = 0;
static void*                    s_segv_fp = nullptr;

/* ROOT FIX (ghfix7): the checkpoint used to be a function wrapping sigsetjmp,
 * but a jmp_buf saved inside a wrapper dies with the wrapper's own frame the
 * moment it returns - every later longjmp landed in DEAD stack. Depending on
 * how badly the dead frame had been overwritten, the "recovered" run either
 * fell through the wrapper's stale return path or jumped to garbage, and in
 * five captured runs it NEVER once executed a checkpoint branch (zero "faulted"
 * logs, zero vm_reset_pending calls): vm_run frames unwound by the jump kept
 * their pending_save_t linked, and the next gc walked the dead chain. setjmp
 * must expand in the CALLER's frame, so the checkpoint is a macro again. */
#define eweb_segv_checkpoint_tag(id)                                     \
    (eweb_setjmp(s_segv_jmp) == 0                                        \
        ? (s_segv_armed = 1, s_segv_id = (id),                           \
           s_segv_fp = __builtin_frame_address(0), 0)                    \
        : (s_segv_armed = 0, 1))
#define eweb_segv_checkpoint() eweb_segv_checkpoint_tag(0)
extern "C" void eweb_segv_disarm(void) { s_segv_armed = 0; }
extern "C" int eweb_segv_try_recover(void)
{
    /* TEMP DIAGNOSTIC: EWEB_SEGV_NORECOVER=1 disables the checkpoint longjmp so
     * the port's signal handler falls through to its full register + fpwalk +
     * backtrace dump and _exit. Used to capture the faulting stack of a crash
     * that recovery otherwise hides (only a one-line [crasharm] is logged for a
     * recovered fault). Remove with the other temp probes. */
    static int s_norecover = -1;
    if(s_norecover < 0) s_norecover = (getenv("EWEB_SEGV_NORECOVER") != nullptr) ? 1 : 0;
    if(s_norecover) return 0;
    if(!s_segv_armed) return 0;   /* no checkpoint: let the handler dump+abort */
    /* TEMP DIAG (ghfix6): log which checkpoint we jump to and whether the fault
     * frame is still below it (fp comparison on a down-growing stack). */
    fprintf(stderr, "[SEGVDIAG] recover to id=%d arm_fp=%p cur_fp=%p %s\n",
        s_segv_id, s_segv_fp, __builtin_frame_address(0),
        ((uintptr_t)__builtin_frame_address(0) < (uintptr_t)s_segv_fp) ? "DEEPER(ok)" : "SHALLOWER(dead-frame!)");
    s_segv_armed = 0;
    eweb_longjmp(s_segv_jmp, 1);
    return 1;   /* not reached */
}

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

/* Flipped to 1 the first time a react-query observer subscribes (the QADD
 * probe), i.e. once the initial hydration render has committed and flushed
 * passive effects. jsnative's js_web_fetch_poll defers async-fetch settlement
 * until then so the query cache cannot flip mid-hydration. C linkage so the C
 * side sees the same symbol. */
extern "C" int g_eweb_first_commit = 0;
/* Count of commitRoot probes seen after the first async-fetch spawn. With
 * EWEB_FETCH_GATE_N=K the settlement gate opens only at the K-th such commit,
 * letting a test push data settlement past the (slow) hydration commit so the
 * deferred update lands as a pure client render instead of interrupting
 * hydration (#461 -> stuck isHydrating -> mismatch-throw discard). */
extern "C" int g_eweb_commit_count = 0;
/* Set by jsnative on the first async-fetch spawn; combined with the commitRoot
 * probe below so the gate opens at the first commit AFTER a suspended render
 * issued its query (a commit before any fetch is a pre-hydration partial render
 * and opening the gate there reproduces the settle-before-hydration
 * mismatch). */
extern "C" int g_eweb_fetch_spawned;

static void* js_platform_malloc(uint32_t size) { return malloc((size_t)size); }
static void  js_platform_free(void* p)         { free(p); }

static void  js_platform_out(const char* s)
{
    if(s == nullptr) return;
    /* TEMP DIAGNOSTIC (taobao): the port routes JS console text through
     * sys.log -> SDL_Log at INFO priority, which a release build suppresses,
     * so console.log/warn/error AND the engine's "Uncaught ..." reports are
     * invisible on stderr. Mirror them to stderr when EWEB_JSMIRROR is set so
     * a failing bundle can be diagnosed. Remove with the other temp probes. */
    static int s_mirror = -1;
    if(s_mirror < 0) s_mirror = (getenv("EWEB_JSMIRROR") != nullptr) ? 1 : 0;
    if(s_mirror) { fputs("[js] ", stderr); fputs(s, stderr); fflush(stderr); }
    /* First-commit signal for the async-fetch settlement gate. QADD (react-query
     * observer subscribe) used to be the only trigger, but after a github
     * redeploy the react-query core moved out of the chunk the QADD probe
     * patches, so the gate never opened early and every deferred query settled
     * at the 60 s fallback deadline - long after hydration had committed with
     * the data pending. commitRoot (probe C, react-dom-client, chunk-stable)
     * fires on every commit including the initial hydration one, so accept it
     * as the signal too. */
    {
        static int s_gate_n = -1;
        if(s_gate_n < 0) {
            const char* gn = getenv("EWEB_FETCH_GATE_N");
            s_gate_n = gn ? atoi(gn) : 0;
        }
        if(strstr(s, "[MODP] CROOT") != nullptr && g_eweb_fetch_spawned != 0)
            g_eweb_commit_count++;
        if(s_gate_n > 0) {
            if(g_eweb_commit_count >= s_gate_n) g_eweb_first_commit = 1;
        } else if(g_eweb_first_commit == 0 &&
                  ((strstr(s, "[MODP] QADD") != nullptr) ||
                   (strstr(s, "[MODP] CROOT") != nullptr && g_eweb_fetch_spawned != 0))) {
            g_eweb_first_commit = 1;
        }
    }
    if(s_js_log_fn == nullptr) return;
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

/* Defined further down with the bridge predicates; declared here (identical to
 * the later forward declarations) so js_set_element_text can classify a node. */
static bool jsElIsPseudo(void* el);
static bool jsElIsTextRun(void* el);
/* EWEB_DOMSIG childNodes signature dump (defined further down). */
static void ewebDumpDomSig(litehtml::element* e, int depth);
/* EWEB_HEADDUMP: head child chain with full attribute tables (defined with
 * ewebDumpDomSig) so the hydration-time head diffs node-by-node vs raw SSR. */
static void ewebDumpHeadAttrs(litehtml::element* head);
/* EWEB_MUTDBG tree-mutation trace (defined with jsElAppendChild below). */
static void mutdbg_log(const char* op, litehtml::element* p, litehtml::element* c, litehtml::element* r);

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
    const char* value = (text != nullptr) ? text : "";
    /* CharacterData nodes carry their value IN the node, so `data`/`nodeValue`
     * assignment REPLACES it. The container path below (drop the children, append
     * one el_text) is only correct for element nodes: applied to a comment it
     * leaves the marker text untouched, which breaks React 19's streaming reveal
     * ($RC writes `template.previousSibling.data = "$~"`, $RV writes the boundary
     * end marker `data = "$"` - with the markers unreadable the hidden S:N
     * segments are never moved into their B:N boundary and the page stays blank
     * or renders without those sections); applied to a text node it nests a child
     * instead of editing the run, and jsElGetText would then report the old tail
     * appended to the new value. */
    if(e->is_comment()) {
        e->set_data_replace(value);
        return;
    }
    if(jsElIsTextRun((void*)e)) {
        e->set_data_replace(value);
        e->parse_styles(false);   /* set_text() dropped the cached measure */
        /* jsElGetText coalesces a parser-split run into ONE browser text node, so
         * the continuation chunks must read as empty. */
        litehtml::element* p = (litehtml::element*)(void*)e->parent();
        if(p != nullptr) {
            bool started = false;
            for(size_t i = 0; i < p->get_children_count(); i++) {
                litehtml::element* c = (litehtml::element*)(void*)p->get_child(i);
                if(c == nullptr) continue;
                if(!started) { if(c == e) started = true; continue; }
                if(jsElIsPseudo((void*)c)) continue;
                if(!jsElIsTextRun((void*)c)) break;   /* run ended */
                c->set_data_replace("");
                c->parse_styles(false);
            }
        }
        return;
    }
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
        mutdbg_log("innerHTML-", e, (litehtml::element*)c, nullptr);
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
        mutdbg_log("innerHTML+", e, n, nullptr);
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

    /* EWEB_NOVCACHE / EWEB_NOLNCACHE: decisive A/B knobs to size mario's two
     * runtime caches to 0 (disabled) without a rebuild churn. var_cache shares
     * literal var_t* by ref (try_var_cache rewrites a literal instruction to
     * INSTR_CACHE(index) on 2nd exec); load_ncache caches resolved binding nodes
     * inside loop scope. If disabling var_cache makes apple /mac render, the
     * blank is a literal-cache aliasing corruption. */
    int vcSize = (getenv("EWEB_NOVCACHE") != nullptr) ? 0 : VAR_CACHE_MAX_DEF;
    int ncSize = (getenv("EWEB_NOLNCACHE") != nullptr) ? 0 : LOAD_NCACHE_MAX_DEF;
    m_jsVm = vm_new(js_compile, vcSize, ncSize);
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
    /* Let __await() pump the event loop for a not-yet-settled promise instead
     * of yielding undefined (see jsAwaitPendingTick). */
    m_jsVm->on_await_pending = jsAwaitPendingTick;
    /* ES module loading: static/dynamic imports fetch their source over the
     * network (see jsModuleResolve/jsModuleLoad). Process-wide in mario. */
    _resolve_m_func = jsModuleResolve;
    _load_m_func    = jsModuleLoad;

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
    /* EWEB_MATCHES_FALLBACK=1 drops the single-element matches callback so the
     * bridge falls back to answering matches() through query_all - an escape
     * hatch if a page ever misbehaves only under the direct select() path. */
    cb.el_matches        = (getenv("EWEB_MATCHES_FALLBACK") != NULL) ? NULL : jsElMatches;
    cb.create_element    = jsCreateElement;
    cb.create_text_node  = jsCreateTextNode;
    cb.create_comment    = jsCreateComment;
    /* Tree walking, mutation, geometry and computed style. */
    cb.el_parent         = jsElParent;
    cb.el_child_count    = jsElChildCount;
    cb.el_child          = jsElChild;
    cb.el_is_tag         = jsElIsTag;
    cb.el_is_comment     = jsElIsComment;
    cb.el_is_live        = jsElIsLive;
    cb.el_append_child   = jsElAppendChild;
    cb.el_insert_before  = jsElInsertBefore;
    cb.el_remove_child   = jsElRemoveChild;
    cb.el_clone_node     = jsElCloneNode;
    cb.el_remove_attr    = jsElRemoveAttr;
    cb.el_get_rect       = jsElGetRect;
    cb.el_transform_rect = jsElTransformRect;
    cb.el_offset_parent_origin = jsElOffsetParentOrigin;
    cb.el_from_point = jsElFromPoint;
    cb.el_get_client_size = jsElGetClientSize;
    cb.get_scroll        = jsGetScroll;
    cb.get_document_size = jsGetDocumentSize;
    cb.get_viewport      = jsWebGetViewport;
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
    /* WebAssembly: registers the WebAssembly namespace + Module/Instance/
     * Memory/Table/Global on the VM. Registered last so window/DOM exist. */
    js_register_wasm_natives(m_jsVm);
}

void EWebEngine::resetJsVm()
{
    if(m_jsVm == nullptr) return;
    vm_close(m_jsVm);      /* frees the VM and everything it owns */
    m_jsVm = nullptr;
}

/* Find/replace patches for page chunks that run through jsRunScriptBody
 * (entry / type="module" <script> tags such as github's code-view chunk)
 * instead of jsModuleLoad, so the same functional rewrites can land in those
 * bundles too. Mirrors the module-path patch loop in jsModuleLoad. */
/* app-foundation zone-info store: mario's useSyncExternalStore mis-drives this
 * subscribe/getSnapshot/getServerSnapshot trio (the SSR snapshot wins and store
 * changes never propagate), leaving dependent providers stuck on their skeleton
 * state. Calling the getSnapshot argument directly (the "forced browser" path)
 * matches what a standard browser observes once hydrated. Matched structurally
 * (3 short args after the call open) so minified renames in bundle redeployments
 * cannot silently disable it. Returns the rewrite count. */
static int ziForceBrowserRewrite(std::string& out)
{
    const char* kUse = ".useSyncExternalStore)(";
    size_t p = out.find(kUse);
    int rw = 0;
    while(p != std::string::npos) {
        size_t q = p + strlen(kUse);          /* first arg char */
        size_t c1 = out.find(',', q);
        size_t c2 = (c1 != std::string::npos) ? out.find(',', c1 + 1) : std::string::npos;
        size_t ce = (c2 != std::string::npos) ? out.find(')', c2 + 1) : std::string::npos;
        if(c1 != std::string::npos && c2 != std::string::npos && ce != std::string::npos &&
           c1 - q <= 4 && c2 - c1 <= 4 && ce - c2 <= 4) {
            std::string snap = out.substr(c1 + 1, c2 - c1 - 1);
            size_t st = out.rfind("(0,", p);
            if(st != std::string::npos && p - st <= 8) {
                /* keep a separator after the enclosing `return` keyword */
                out.replace(st, (ce + 1) - st, " " + snap + "()");
                rw++;
                p = out.find(kUse, st + snap.size() + 3);
                continue;
            }
        }
        p = out.find(kUse, p + 1);
    }
    return rw;
}
/* Minified react jsx-factory call opens look like "(0,<ns>.jsx)(" /
 * "(0,<ns>.jsxs)("; the namespace identifier is renamed on every github bundle
 * redeployment (h -> f -> ...), so it must never be hardcoded. isJsxOpenAt
 * reports whether a factory call open starts at i; the find/rfind wrappers
 * locate the nearest one. */
static bool jsxIdentCh(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='$'; }
static bool isJsxOpenAt(const std::string& s, size_t i)
{
    if(i + 6 >= s.size() || s[i] != '(' || s[i+1] != '0' || s[i+2] != ',') return false;
    size_t j = i + 3;
    if(!jsxIdentCh(s[j])) return false;
    while(j < s.size() && jsxIdentCh(s[j])) j++;
    if(j + 5 < s.size() && s[j]=='.' && s[j+1]=='j' && s[j+2]=='s' && s[j+3]=='x' &&
       s[j+4]==')' && s[j+5]=='(') return true;                 /* .jsx)( */
    if(j + 6 < s.size() && s[j]=='.' && s[j+1]=='j' && s[j+2]=='s' && s[j+3]=='x' &&
       s[j+4]=='s' && s[j+5]==')' && s[j+6]=='(') return true;  /* .jsxs)( */
    return false;
}
static size_t findJsxOpen(const std::string& s, size_t from)
{
    for(size_t i = from; i + 6 < s.size(); ++i) if(isJsxOpenAt(s, i)) return i;
    return std::string::npos;
}
static size_t rfindJsxOpen(const std::string& s, size_t before)
{
    if(before > s.size()) before = s.size();
    for(size_t i = before; i-- > 0; ) if(isJsxOpenAt(s, i)) return i;
    return std::string::npos;
}
/* Index just past the '(' that opens the factory call's argument list. */
static size_t jsxOpenArgStart(const std::string& s, size_t i)
{
    size_t j = i + 3;
    while(j < s.size() && jsxIdentCh(s[j])) j++;
    while(j < s.size() && s[j] != '(') j++;
    return (j < s.size()) ? j + 1 : std::string::npos;
}
static std::string jsScriptModPatch(const std::string& url, const std::string& in)
{
    /* Functional (non-diagnostic) patches: applied unconditionally because they
     * paper over an engine deficiency that otherwise leaves github's file table
     * stuck on its SSR skeleton. The "directory-content" boundary wrapper around
     * the file-table component never lets it mount under mario; rendering the
     * boundary's inner child directly fills the commit message/date columns.
     * Matched structurally (by boundaryName) so github bundle redeployments
     * (which rename the minified identifiers) cannot silently disable it. */
    std::string out = in;
    /* EWEB_RERRDBG: instrument React's production error decoder so the
     * minified #418/#423 throws carry their arguments AND a JS stack: the
     * decoder is the single funnel every React error passes through, so one
     * rewrite here decodes which internal path (hydration mismatch vs
     * recovery vs root failure) fired and from where. */
    if(getenv("EWEB_RERRDBG") != nullptr) {
        const char* kDec = "var t=\"https://react.dev/errors/\"+e;";
        size_t p = out.find(kDec);
        while(p != std::string::npos) {
            static const char* kIns =
                "try{console.error('[RERR]#'+e+' a='+String(arguments[1])+'|'+String(arguments[2])+ ' st='+new Error().stack);}catch(z){}";
            out.insert(p + strlen(kDec), kIns);
            fprintf(stderr, "[modpatch] RERRDBG decoder instrumented in %s\n", url.c_str());
            p = out.find(kDec, p + strlen(kDec) + strlen(kIns));
        }
    }
    /* EWEB_USETHEN: React's use(thenable) suspend point is `throw nY=t,n$`
     * (nY=the thenable being suspended on, n$=the #460 control-flow Error).
     * Log the thenable's status/ctor and the JS stack so the component/hook
     * whose data promise never settles during hydration is named. The stack's
     * pcs map onto chunks via the pc_range table (EWEB_SCRIPTDBG). */
    if(getenv("EWEB_USETHEN") != nullptr) {
        const char* kUse = "throw nY=t,n$";
        size_t p = out.find(kUse);
        if(p != std::string::npos) {
            static const char* kIns =
                "throw (function(){try{console.error('[USETHEN] status='+t.status+"
                "' ctor='+(t&&t.constructor?t.constructor.name:'-')+"
                "' st='+new Error().stack);}catch(z){}nY=t;return n$;})()";
            out.replace(p, strlen(kUse), kIns);
            fprintf(stderr, "[modpatch] USETHEN instrumented in %s\n", url.c_str());
        }
    }
    /* EWEB_SECDBG: apple's content resolver (module 97615 `H`) wraps the
     * layoutData->node resolution in a SILENT `try{...}catch{}` that returns
     * undefined on any throw. The page component's `sections` prop comes from
     * this resolver, so a swallowed throw => undefined result => `e.sections`
     * missing => `e.sections.map` throws inside React render => React 19 folds
     * it into a fatal #418 and never commits <main> => blank body. Instrument
     * the resolver to log the contentId, whether the LayoutDataContext ref is
     * populated, and the resolved _model/sections length on success OR the
     * thrown message on failure. Behavior-preserving: the catch still swallows
     * (returns undefined) exactly as before, only console.log is added. */
    if(getenv("EWEB_SECDBG") != nullptr) {
        const char* kH =
            "try{let e=n(t,d.current.current,r,c[0].current);"
            "return{dependents:c[0],result:e}}catch{}";
        size_t p = out.find(kH);
        if(p != std::string::npos) {
            static const char* kIns =
                "try{let e=n(t,d.current.current,r,c[0].current);"
                "try{console.log('[SECDBG]OK cid='+t+"
                "' ld='+(void 0===d.current.current?'UNDEF':'ok')+"
                "' res='+(void 0===e?'UNDEF':(e&&e._model?e._model:typeof e))+"
                "' sec='+(e&&Array.isArray(e.sections)?e.sections.length:"
                "(e&&void 0!==e.sections?'nonarr':'none')));}catch(z){}"
                "return{dependents:c[0],result:e}}"
                "catch(err){try{console.log('[SECDBG]THROW cid='+t+"
                "' ld='+(void 0===d.current.current?'UNDEF':'ok')+"
                "' msg='+(err&&err.message?err.message:String(err)));}catch(z){}}";
            out.replace(p, strlen(kH), kIns);
            fprintf(stderr, "[modpatch] SECDBG H instrumented in %s\n", url.c_str());
        }
    }
    /* EWEB_HYDNODE: at React's hydration-mismatch throw site (minified t5,
     * `throw Error(i(418))`) the offending DOM node is the module-level tG and
     * the parent fiber tX, neither reachable from outside. Log both before the
     * throw so the exact mismatched node is named instead of guessed. */
    if(getenv("EWEB_HYDNODE") != nullptr) {
        const char* kT5 = "function t5(){throw Error(i(418))}";
        size_t p = out.find(kT5);
        if(p != std::string::npos) {
            static const char* kIns =
                "function t5(fb){try{console.error('[HYDNODE] nt='+(tG?tG.nodeType:'-')"
                "+' tag='+(tG?tG.tagName:'-')"
                "+' txt='+(tG&&tG.nodeValue?String(tG.nodeValue).slice(0,40):'-')"
                "+' ptag='+(tG&&tG.parentNode?tG.parentNode.tagName:'-')"
                "+' ftag='+(fb?fb.tag:'-')+' ftype='+(fb&&fb.type?fb.type:'-')"
                "+' ffl='+(fb?fb.flags:'-')"
                "+' fdehy='+(fb&&fb.memoizedState?String(!!fb.memoizedState.dehydrated):'-')"
                "+' pftag='+(tX?tX.tag:'-')+' pftype='+(tX&&tX.type?tX.type:'-'));}catch(z){}"
                "throw Error(i(418))}";
            out.replace(p, strlen(kT5), kIns);
            /* Forward the work-in-progress fiber at each mismatch call site so
             * t5 can name the EXPECTED element alongside the actual node. */
            static const char* kCT = "t8(t)&&t5()";
            size_t q = out.find(kCT);
            while(q != std::string::npos) {
                out.replace(q, strlen(kCT), "t8(t)&&t5(t)");
                q = out.find(kCT, q + 11);
            }
            static const char* kCE = "t8(e)&&t5()";
            q = out.find(kCE);
            while(q != std::string::npos) {
                out.replace(q, strlen(kCE), "t8(e)&&t5(e)");
                q = out.find(kCE, q + 11);
            }
            fprintf(stderr, "[modpatch] HYDNODE t5 instrumented in %s\n", url.c_str());
            /* Log the four conjuncts of hydrateSuspense's recoverable
             * in-place-client-render branch so the one that fails is named. */
            const char* kSus = "case 13:if(ro(t),r=t.memoizedState,null===e||";
            size_t s = out.find(kSus);
            if(s != std::string::npos) {
                static const char* kSusIns =
                    "case 13:if(ro(t),r=t.memoizedState,(function(){try{console.error('[HYDSUS] tZ='+tZ+"
                    "' tG='+(tG?(tG.tagName||tG.nodeType):'null')+' mode='+t.mode+' flags='+t.flags+"
                    "' rdehy='+(r?String(!!r.dehydrated):'-')+' cur='+(e?'x':'null'));}catch(z){}})(),null===e||";
                out.replace(s, strlen(kSus), kSusIns);
                fprintf(stderr, "[modpatch] HYDSUS entry instrumented in %s\n", url.c_str());
            }
            /* t9 entry: name which fiber/tag enters the mismatch guard and the
             * hydration state at that moment, to attribute the fatal throw. */
            const char* kT9 = "function t9(e){if(e!==tX)return!1;";
            size_t t9p = out.find(kT9);
            if(t9p != std::string::npos) {
                static const char* kT9Ins =
                    "function t9(e){try{console.error('[HYDT9] etag='+e.tag+' eq='+(e===tX)+"
                    "' tZ='+tZ+' tG='+(tG?(tG.tagName||tG.nodeType):'null'));}catch(z){}"
                    "if(e!==tX)return!1;";
                out.replace(t9p, strlen(kT9), kT9Ins);
                fprintf(stderr, "[modpatch] HYDT9 entry instrumented in %s\n", url.c_str());
            }
            /* case 27 (HostSingleton html/head/body) hydration entry: log the
             * singleton type and the current hydration pointer so a head-walk
             * desync (stale tG) is visible against the SSR head sequence. */
            const char* k27 = "case 27:if(j(t),n=I.current,l=t.type,";
            size_t p27 = out.find(k27);
            if(p27 != std::string::npos) {
                static const char* k27Ins =
                    "case 27:if(j(t),n=I.current,l=t.type,(function(){try{console.error('[HYD27] type='+l+"
                    "' tZ='+tZ+' tG='+(tG?(tG.tagName||tG.nodeType):'null'));}catch(z){}})(),";
                out.replace(p27, strlen(k27), k27Ins);
                fprintf(stderr, "[modpatch] HYD27 entry instrumented in %s\n", url.c_str());
            }
        }
        /* React 19.1.x (apple artisan framework chunk) hydration-mismatch throw
         * rP(fiber): the offending DOM node is the module-level hydration pointer
         * rS and the expected side's parent fiber is rw; neither is reachable
         * from outside. Log fiber + node + hydration state before the throw. */
        const char* kRP = "function rP(e){throw rR(nZ(Error(u(418,\"\")),e)),r_}";
        size_t rp = out.find(kRP);
        /* EWEB_MMX = rP-only probe (fires once at the throw, negligible cost) so
         * the #418 mismatch can be observed WITHOUT the rT walk slowdown that
         * masks it. EWEB_MMXFW still enables both (legacy). */
        if(rp != std::string::npos && (getenv("EWEB_MMX") != nullptr || getenv("EWEB_MMXFW") != nullptr)) {
            static const char* kRPIns =
                "function rP(e){try{console.error('[MMX] ftag='+(e?e.tag:'-')"
                "+' ftype='+(e&&e.type?(e.type.name||e.type):'-')"
                "+' ffl='+(e?e.flags:'-')"
                "+' fdehy='+(e&&e.memoizedState?String(!!e.memoizedState.dehydrated):'-')"
                "+' nt='+(rS?rS.nodeType:'-')+' tag='+(rS&&rS.tagName?rS.tagName:'-')"
                "+' txt='+(rS&&rS.nodeValue?String(rS.nodeValue).slice(0,60):'-')"
                "+' ptag='+(rS&&rS.parentNode?(rS.parentNode.tagName||rS.parentNode.nodeType):'-')"
                "+' pwtag='+(rw?rw.tag:'-')"
                "+' pwtype='+(rw&&rw.type?(rw.type.name||rw.type):'-')"
                "+' rx='+rx+' rC='+rC);}catch(z){}"
                "throw rR(nZ(Error(u(418,\"\")),e)),r_}";
            out.replace(rp, strlen(kRP), kRPIns);
            fprintf(stderr, "[modpatch] MMX rP instrumented in %s\n", url.c_str());
        }
        /* EWEB_MMXBIG=<N>: CONTROL experiment, size sweep. Insert N purely-inert
         * `var _zK=0` declarations into rP's body (no strings, no try/catch, no
         * calls, no side effects). rP is NEVER called on the render path, so the
         * only way this can flip blank->render is by shifting the bytecode
         * offsets of every function AFTER rP (rT, the hydration walk, etc).
         * MMXBIG N=52 (net +317B) did NOT flip; the full MMX probe (net +500B)
         * DID. Sweeping N disambiguates a pure SIZE/OFFSET threshold (some N>=T
         * flips -> mario compiler/VM offset-encoding bug) from a CONTENT effect
         * (no inert N ever flips -> the try/catch or console.error or the string
         * literals in MMX are load-bearing). */
        if(getenv("EWEB_MMXBIG") != nullptr) {
            size_t rpb = out.find(kRP);
            if(rpb != std::string::npos) {
                int n = atoi(getenv("EWEB_MMXBIG"));
                if(n < 1) n = 52;
                std::string blob = "function rP(e){var ";
                for(int i = 0; i < n; ++i) {
                    char nm[24];
                    snprintf(nm, sizeof(nm), "%s_z%d=0", i ? "," : "", i);
                    blob += nm;
                }
                blob += ";throw rR(nZ(Error(u(418,\"\")),e)),r_}";
                out.replace(rpb, strlen(kRP), blob);
                fprintf(stderr, "[modpatch] MMXBIG n=%d (%zu bytes, net+%zd) in %s\n",
                        n, blob.size(),
                        (ptrdiff_t)blob.size() - (ptrdiff_t)strlen(kRP), url.c_str());
            }
        }
        /* EWEB_MMXVAR / MMXTRY / MMXSTR: CONTENT-BISECT probes. MMXBIG proved the
         * flip is NOT size (inert net+3094B still blank) and the _FAIL dump
         * proved it is NOT a compile break (0 failures). So one specific CONTENT
         * element of the MMX probe suppresses rT's `t&&rS&&rP(e)` #418 call.
         * Isolate each element in an otherwise-inert rP body (no console.error,
         * nothing runs while rP is uncalled):
         *   MMXVAR: reference the module-scope hydration bindings rS/rw/rx/rC/rE
         *           (declared `var rw=null,rS=null,rx=!1,rE=null,rC=!1` in the
         *           SAME scope as rP and rT; rT already reads rw/rS/rx but the
         *           original rP does NOT). If this alone flips, merely capturing
         *           a module var in a nested fn changes how rT resolves it =>
         *           mario closure-conversion / scope-resolution bug.
         *   MMXTRY: an empty try/catch (adds an exception-table entry).
         *   MMXSTR: string literals only (string-pool / interning effect). */
        if(getenv("EWEB_MMXVAR") != nullptr) {
            size_t q = out.find(kRP);
            if(q != std::string::npos) {
                static const char* kV =
                    "function rP(e){var _a=rS,_b=rw,_c=rx,_d=rC,_f=rE;"
                    "throw rR(nZ(Error(u(418,\"\")),e)),r_}";
                out.replace(q, strlen(kRP), kV);
                fprintf(stderr, "[modpatch] MMXVAR module-var refs in %s\n", url.c_str());
            }
        }
        if(getenv("EWEB_MMXTRY") != nullptr) {
            size_t q = out.find(kRP);
            if(q != std::string::npos) {
                static const char* kT =
                    "function rP(e){try{}catch(z){}"
                    "throw rR(nZ(Error(u(418,\"\")),e)),r_}";
                out.replace(q, strlen(kRP), kT);
                fprintf(stderr, "[modpatch] MMXTRY empty try/catch in %s\n", url.c_str());
            }
        }
        if(getenv("EWEB_MMXSTR") != nullptr) {
            size_t q = out.find(kRP);
            if(q != std::string::npos) {
                static const char* kS =
                    "function rP(e){var _s='[pb-aaaa][pb-bbbb][pb-cccc][pb-dddd][pb-eeee]';"
                    "throw rR(nZ(Error(u(418,\"\")),e)),r_}";
                out.replace(q, strlen(kRP), kS);
                fprintf(stderr, "[modpatch] MMXSTR string literals in %s\n", url.c_str());
            }
        }
        /* EWEB_MMXCALL / MMXSTRS / MMXACC: second-round content bisect. Round 1
         * (MMXVAR module-var refs / MMXTRY empty try-catch / MMXSTR one string)
         * all stayed blank, so the MMX flip needs the COMBINATION. Isolate the
         * three remaining untested elements of the real MMX probe:
         *   MMXCALL: a console.error CALL (the only side-effecting call in MMX;
         *            host C layer strstr-consumes console output for fetch gates).
         *   MMXSTRS: MANY (15) distinct string literals (string-table growth /
         *            interning), no call, no try.
         *   MMXACC: property accesses + nested ternaries on the param e (the
         *            expression FORM), no call, no added strings. */
        if(getenv("EWEB_MMXCALL") != nullptr) {
            size_t q = out.find(kRP);
            if(q != std::string::npos) {
                static const char* kC =
                    "function rP(e){console.error('[cbcall]');"
                    "throw rR(nZ(Error(u(418,\"\")),e)),r_}";
                out.replace(q, strlen(kRP), kC);
                fprintf(stderr, "[modpatch] MMXCALL console.error call in %s\n", url.c_str());
            }
        }
        if(getenv("EWEB_MMXSTRS") != nullptr) {
            size_t q = out.find(kRP);
            if(q != std::string::npos) {
                static const char* kSS =
                    "function rP(e){var _s0='a0',_s1='a1',_s2='a2',_s3='a3',_s4='a4',"
                    "_s5='a5',_s6='a6',_s7='a7',_s8='a8',_s9='a9',_sa='aa',_sb='ab',"
                    "_sc='ac',_sd='ad',_se='ae';"
                    "throw rR(nZ(Error(u(418,\"\")),e)),r_}";
                out.replace(q, strlen(kRP), kSS);
                fprintf(stderr, "[modpatch] MMXSTRS 15 string literals in %s\n", url.c_str());
            }
        }
        if(getenv("EWEB_MMXACC") != nullptr) {
            size_t q = out.find(kRP);
            if(q != std::string::npos) {
                static const char* kA =
                    "function rP(e){var _x=e?e.tag:0,_y=e&&e.type?e.type.name:0,"
                    "_z=e&&e.memoizedState?e.memoizedState.dehydrated:0;"
                    "throw rR(nZ(Error(u(418,\"\")),e)),r_}";
                out.replace(q, strlen(kRP), kA);
                fprintf(stderr, "[modpatch] MMXACC property-access/ternary in %s\n", url.c_str());
            }
        }
        /* EWEB_MMXFILE=<path>: read rP's ENTIRE replacement function source from
         * a file and splice it over the original rP. This makes the MMX-flip
         * bisection rebuild-free: edit the file, re-run. The file must hold a
         * complete `function rP(e){...}` (it replaces kRP verbatim). Used to
         * narrow which clause of the full MMX probe is load-bearing for the
         * blank->render flip (rP never executes on the render path, so the flip
         * is a COMPILE-TIME effect of rP's body on the rest of the module). */
        if(getenv("EWEB_MMXFILE") != nullptr) {
            size_t q = out.find(kRP);
            if(q != std::string::npos) {
                FILE* fp = fopen(getenv("EWEB_MMXFILE"), "rb");
                if(fp != nullptr) {
                    std::string body;
                    char buf[4096];
                    size_t nr;
                    while((nr = fread(buf, 1, sizeof(buf), fp)) > 0) body.append(buf, nr);
                    fclose(fp);
                    if(!body.empty()) {
                        out.replace(q, strlen(kRP), body);
                        fprintf(stderr, "[modpatch] MMXFILE (%zu bytes) from %s in %s\n",
                                body.size(), getenv("EWEB_MMXFILE"), url.c_str());
                    }
                }
            }
        }
        /* rT guard entry: name each fiber entering the mismatch guard and the
         * live hydration pointer, to attribute which comparison failed. */
        const char* kRT = "function rT(e){if(e!==rw)return!1;";
        size_t rtp = out.find(kRT);
        /* EWEB_MMXT = rT guard probe (3028x per hydration walk; the console.error
         * cost itself perturbs timing and can MASK the #418). Kept separate from
         * the cheap rP probe so mismatch observation need not slow the walk. */
        if(rtp != std::string::npos && (getenv("EWEB_MMXT") != nullptr || getenv("EWEB_MMXFW") != nullptr)) {
            static const char* kRTIns =
                "function rT(e){try{console.error('[MMXT] etag='+(e?e.tag:'-')"
                "+' etype='+(e&&e.type?(e.type.name||e.type):'-')"
                "+' eq='+(e===rw)+' rx='+rx"
                "+' rS='+(rS?(rS.tagName||rS.nodeType):'null'));}catch(z){}"
                "if(e!==rw)return!1;";
            out.replace(rtp, strlen(kRT), kRTIns);
            fprintf(stderr, "[modpatch] MMXT rT instrumented in %s\n", url.c_str());
        }
        /* renderWithHooks (lU) invocation site `a=n(r,l)` (n=Component, r=props):
         * when React renders a function component with UNDEFINED props (the apple
         * blank-page signature - pageFn does e.sections.map and throws), log the
         * fiber context to attribute the reconciler path (mount vs update, flags,
         * lane, dehydrated, parent tag). Inserted as an extra element of lU's own
         * comma-sequence so mario's parser sees the same shape. Behavior-preserving
         * (log only, and only on the rare undefined-props path). */
        const char* kRWH = "lO=!1,a=n(r,l),lO=!1";
        size_t rwhp = out.find(kRWH);
        if(rwhp != std::string::npos && getenv("EWEB_RWH") != nullptr) {
            static const char* kRWHIns =
                "lO=!1,globalThis.__rwh=(globalThis.__rwh|0)+1,"
                "1===globalThis.__rwh&&console.error('[RWH]live first tag='+t.tag),"
                "void 0===r&&console.error('[RWH]undef tag='+t.tag"
                "+' mnt='+(null===e)+' fl='+t.flags+' ln='+t.lanes"
                "+' dehy='+(t.memoizedState&&t.memoizedState.dehydrated?1:0)"
                "+' par='+(t.return?t.return.tag:'-')"
                "+' sib='+(null!==t.sibling)+' alt='+(null!==t.alternate)"
                "+' n='+globalThis.__rwh)"
                ",a=n(r,l),lO=!1";
            out.replace(rwhp, strlen(kRWH), kRWHIns);
            fprintf(stderr, "[modpatch] RWH renderWithHooks instrumented in %s\n", url.c_str());
        }
        /* React 19 sync-work-loop Suspense REPLAY path (the route that BYPASSES
         * renderWithHooks/lU, so the EWEB_RWH probe never sees it). When a
         * component suspends on a use()/thenable, the work loop records the
         * thrown thenable in iR and the SuspendedReason in iO; on the next
         * iteration, if lr(iR) reports the thenable settled, it calls uf(iT) to
         * REPLAY the suspended fiber. uf's tag-switch re-invokes function
         * components via oC=replayFunctionComponent -> lH=renderWithHooksAgain,
         * passing t.pendingProps as the props. This is the ONLY component-invoke
         * path that does not go through lU, so it is the prime suspect for the
         * apple blank-page signature (pageFn re-invoked with undefined props ->
         * e.sections.map throws -> #418). Two probes, both log ONLY on the rare
         * undefined-props branch, so behavior is preserved:
         *   [UFREPLAY] at uf entry: confirms the replayed fiber's pendingProps is
         *     undefined AND captures WHY it is replaying (iO SuspendedReason, iR
         *     thenable status, rx isHydrating) plus fiber identity.
         *   [REPLAYFN] at oC entry: confirms props===undefined exactly at the
         *     replayFunctionComponent->renderWithHooksAgain invoke boundary and
         *     names the Component. */
        if(getenv("EWEB_REPLAY") != nullptr) {
            const char* kUF = "function uf(e){var t=e,n=t.alternate;";
            size_t ufp = out.find(kUF);
            if(ufp != std::string::npos) {
                static const char* kUFIns =
                    "function uf(e){try{if((0===e.tag||15===e.tag||11===e.tag)"
                    "&&void 0===e.pendingProps)console.error('[UFREPLAY]undef'"
                    "+' tag='+e.tag+' iO='+iO"
                    "+' iRst='+(iR&&void 0!==iR.status?iR.status:typeof iR)"
                    "+' rx='+rx+' fl='+e.flags+' ln='+e.lanes"
                    "+' mp='+(void 0===e.memoizedProps?'U':'set')"
                    "+' alt='+(null!==e.alternate)"
                    "+' dehy='+(e.memoizedState&&e.memoizedState.dehydrated?1:0)"
                    "+' typ='+(e.type&&(e.type.name||e.type))"
                    "+' par='+(e.return?e.return.tag:'-'));}catch(z){}"
                    "var t=e,n=t.alternate;";
                out.replace(ufp, strlen(kUF), kUFIns);
                fprintf(stderr, "[modpatch] REPLAY uf instrumented in %s\n", url.c_str());
            }
            const char* kOC = "function oC(e,t,n,r,l,a){return(rV(t),t.updateQueue=null,";
            size_t ocp = out.find(kOC);
            if(ocp != std::string::npos) {
                static const char* kOCIns =
                    "function oC(e,t,n,r,l,a){try{void 0===n&&"
                    "console.error('[REPLAYFN]undef-props tag='+t.tag"
                    "+' fl='+t.flags+' ln='+t.lanes+' alt='+(null!==e)"
                    "+' mp='+(void 0===t.memoizedProps?'U':'set')"
                    "+' typ='+(r&&(r.name||r)));}catch(z){}"
                    "return(rV(t),t.updateQueue=null,";
                out.replace(ocp, strlen(kOC), kOCIns);
                fprintf(stderr, "[modpatch] REPLAY oC instrumented in %s\n", url.c_str());
            }
        }
        /* chunk 9812 page component `re=u(e=>{...})`: log at body entry whether
         * re is reached and whether injected props carry `sections`. Statement
         * form (no comma-operator/ternary) to keep mario's parser happy. */
        const char* kMM = "re=u(e=>{let t=";
        size_t mm = out.find(kMM);
        if(mm != std::string::npos) {
            static const char* kMMIns =
                "re=u(e=>{try{console.error('[MMR] etype='+typeof e"
                "+' hasSec='+(e&&e.sections!==undefined)"
                "+' secLen='+(e&&e.sections?e.sections.length:-1)"
                "+' nkeys='+Object.keys(e||{}).length);}catch(z){}let t=";
            /* EWEB_MMRSTACK: when the page component is re-invoked with
             * UNDEFINED props (the blank-page signature), capture the JS call
             * stack. A stack rooted at React's scheduler (a fresh root with no
             * outer render frame) proves the re-invocation is a RE-ENTRANT
             * scheduler macrotask fired by jsAwaitPendingTick during an await
             * inside the render path - the browser-divergent path that thrashes
             * the render loop. Behavior-preserving (log only). */
            static const char* kMMInsStk =
                "re=u(e=>{try{console.error('[MMR] etype='+typeof e"
                "+' hasSec='+(e&&e.sections!==undefined)"
                "+' nkeys='+Object.keys(e||{}).length);"
                "console.error('[MMRSTK '+(void 0===e?'UNDEF':'OBJ')+']'"
                "+(new Error().stack));}catch(z){}let t=";
            out.replace(mm, strlen(kMM),
                        getenv("EWEB_MMRSTACK") != nullptr ? kMMInsStk : kMMIns);
            fprintf(stderr, "[modpatch] MMR re-entry instrumented in %s\n", url.c_str());
        }
        /* EWEB_PRELOADDBG: instrument the Next.js main entry's preloadReady gate.
         * The main chunk (8792) has:
         *   window.__NEXT_PRELOADREADY&&await window.__NEXT_PRELOADREADY(a.dynamicIds)
         * If __NEXT_PRELOADREADY is undefined at this point (script order issue),
         * loadable components never pre-resolve → hydration renders loading(null)
         * → #418. Log whether it exists and how many dynamicIds are passed. */
        if(getenv("EWEB_PRELOADDBG") != nullptr) {
            const char* kPR =
                "window.__NEXT_PRELOADREADY&&await window.__NEXT_PRELOADREADY(a.dynamicIds)";
            size_t pr = out.find(kPR);
            if(pr != std::string::npos) {
                static const char* kPRIns =
                    "console.error('[PRELOAD] exists='+(!!window.__NEXT_PRELOADREADY)"
                    "+' ids='+(a.dynamicIds?a.dynamicIds.length:-1)),"
                    "window.__NEXT_PRELOADREADY&&await window.__NEXT_PRELOADREADY(a.dynamicIds),"
                    "console.error('[PRELOAD] after-await')";
                out.replace(pr, strlen(kPR), kPRIns);
                fprintf(stderr, "[modpatch] PRELOADDBG instrumented in %s\n", url.c_str());
            }
        }
        /* module 99812 transform chain that FEEDS props to the page component:
         * `o` (sosumi/contentId transform) resolves content via 97615.H, then
         * `a` merges {result} over props and cloneElement's the page element.
         * MMR shows the page component gets undefined props on intermediate
         * hydration passes -> empty <main> -> #418. Log o's contentId and a's
         * inputs/merged output per pass to see whether H returned undefined or
         * the merge/cloneElement dropped props. Behavior-preserving (log only). */
        const char* kOH =
            "o=({contentId:e,props:t,children:r,next:n})=>"
            "{let o=((e,t=!0)=>(0,l.H)(!1,e,t))(e);";
        size_t oh = out.find(kOH);
        if(oh != std::string::npos) {
            static const char* kOHIns =
                "o=({contentId:e,props:t,children:r,next:n})=>"
                "{let o=((e,t=!0)=>(0,l.H)(!1,e,t))(e);"
                "try{console.error('[OHOC] cid='+e+"
                "' hres='+(o&&void 0!==o.result?(o.result&&o.result._model?o.result._model:'obj'):'UNDEF')+"
                "' ptype='+typeof t);}catch(z){}";
            out.replace(oh, strlen(kOH), kOHIns);
            fprintf(stderr, "[modpatch] OHOC instrumented in %s\n", url.c_str());
        }
        const char* kAM =
            "a=(e,t,r,n)=>{let a,l=e?.result;"
            "return a=void 0===l?t??{}:void 0===t?l:{...l,...t},"
            "n((0,i.cloneElement)(r,a))}";
        size_t am = out.find(kAM);
        if(am != std::string::npos) {
            static const char* kAMIns =
                "a=(e,t,r,n)=>{let a,l=e?.result;"
                "a=void 0===l?t??{}:void 0===t?l:{...l,...t};"
                "try{console.error('[AMERGE] ldef='+(void 0!==l)+"
                "' ttype='+typeof t+' merged='+(a===undefined?'UNDEF':typeof a)+"
                "' nkeys='+Object.keys(a||{}).length+"
                "' hasSec='+(a&&a.sections!==undefined));}catch(z){}"
                "return n((0,i.cloneElement)(r,a))}";
            out.replace(am, strlen(kAM), kAMIns);
            fprintf(stderr, "[modpatch] AMERGE instrumented in %s\n", url.c_str());
        }
        /* page.js `sections-page` is a @loadable dynamic import whose <main>
         * renderer (module 99812) needs chunks [500,4112,2138,8681,9812]. Log
         * each r.e(id) resolve/fail and the Promise.all outcome to see whether
         * the loadable ever settles (re-entry never fired => suspect pending). */
        const char* kCL = "Promise.all([r.e(500),r.e(4112),r.e(2138),r.e(8681),r.e(9812)]).then(r.bind(r,99812))";
        size_t cl = out.find(kCL);
        if(cl != std::string::npos) {
            static const char* kCLIns =
                "Promise.all([r.e(500),r.e(4112),r.e(2138),r.e(8681),r.e(9812)])"
                ".then(function(){console.error('[CLOAD] RESOLVED t='+Date.now());"
                "var f=r.bind(r,99812);"
                "try{var M=f();console.error('[CLOAD] REQ99812 ok type='+typeof M+' t='+Date.now());return M}"
                "catch(q){console.error('[CLOAD] REQ99812 THROW '+q+' t='+Date.now());throw q}},"
                "function(er){console.error('[CLOAD] ALLFAIL '+er);throw er})";
            out.replace(cl, strlen(kCL), kCLIns);
            fprintf(stderr, "[modpatch] CLOAD instrumented in %s\n", url.c_str());
        }
        /* hydrateRoot entry timestamp: establish ordering vs the sections-page
         * @loadable resolve ([CLOAD] RESOLVED) and the #418 throw ([MMX]). If
         * hydrateRoot ENTER precedes CLOAD RESOLVED, the dynamic <main> cannot
         * be present at the first synchronous hydration render -> fatal #418. */
        const char* kHR = "hydrateRoot=function(e,t,n){if(!s(e))throw Error(u(299));";
        size_t hr = out.find(kHR);
        if(hr != std::string::npos) {
            static const char* kHRIns =
                "hydrateRoot=function(e,t,n){console.error('[HYDR] ENTER t='+Date.now());"
                "if(!s(e))throw Error(u(299));";
            out.replace(hr, strlen(kHR), kHRIns);
            fprintf(stderr, "[modpatch] HYDR hydrateRoot instrumented in %s\n", url.c_str());
        }
        /* @openfeature/web-sdk (a dep of module 99812) throws
         * "can not find function 'keys' on object{}" because a base-class method
         * (getUnboundEmitters/buildAndCacheEventEmitterForClient) calls
         * this._domainScopedProviders.keys() while _domainScopedProviders is
         * undefined. It is set ONLY in the subclass ctor comma-chain, after
         * super() and after _defaultProvider=new F(...). Log ctor progress and
         * the field state at the .keys() site to see where it stops. */
        const char* kOF1 = "constructor(){super(\"client\"),this._statusEnumType=R";
        size_t of1 = out.find(kOF1);
        if(of1 != std::string::npos) {
            static const char* kOF1Ins =
                "constructor(){super(\"client\"),console.error('[OF] ctor postSuper'),this._statusEnumType=R";
            out.replace(of1, strlen(kOF1), kOF1Ins);
            fprintf(stderr, "[modpatch] OF ctor-postSuper instrumented in %s\n", url.c_str());
        }
        const char* kOF2 = "this._domainScopedProviders=new Map,this._createEventEmitter=";
        size_t of2 = out.find(kOF2);
        if(of2 != std::string::npos) {
            static const char* kOF2Ins =
                "this._domainScopedProviders=new Map,console.error('[OF] ctor dsp SET ok='+(this._domainScopedProviders instanceof Map)),this._createEventEmitter=";
            out.replace(of2, strlen(kOF2), kOF2Ins);
            fprintf(stderr, "[modpatch] OF ctor-dsp instrumented in %s\n", url.c_str());
        }
        const char* kOF4 = "H=class{constructor(e){this._hooks=[]";
        size_t of4 = out.find(kOF4);
        if(of4 != std::string::npos) {
            static const char* kOF4Ins =
                "H=class{constructor(e){console.error('[OF] H-ctor runsOn='+e),this._hooks=[]";
            out.replace(of4, strlen(kOF4), kOF4Ins);
            fprintf(stderr, "[modpatch] OF H-ctor instrumented in %s\n", url.c_str());
        }
    }
    /* @loadable/component preloadReady recursion fix (functional, unconditional -
     * deliberately OUTSIDE the EWEB_HYDNODE diagnostic gate so clean runs get it
     * too). h(t,e) pops resolvers from t, calls them, then:
     *   Promise.all(r).then(()=>{if(t.length) return h(t,e)})
     * There is NO rejection handler. If ANY store promise rejects (e.g. a
     * mario-missing API causes one loader to throw), Promise.all rejects,
     * the .then() is skipped, and the recursive drain never fires. Newly-
     * registered resolvers (from cascading loadables like module 99812's
     * section components) are never pre-resolved → hydration renders the
     * loading component (null) → SSR DOM unconsumed → React #418.
     * Fix: add a rejection handler that still recurses, matching the
     * real-browser behavior where no store rejects. */
    {
        const char* kHLd =
            "function h(t,e){let r=[];for(;t.length;){let i=t.pop();r.push(i(e))}"
            "return Promise.all(r).then(()=>{if(t.length)return h(t,e)})}";
        size_t hld = out.find(kHLd);
        if(hld != std::string::npos) {
            static const char* kHLdFix =
                "function h(t,e){let r=[];for(;t.length;){let i=t.pop();r.push(i(e))}"
                "return Promise.all(r).then(()=>{if(t.length)return h(t,e)},"
                "()=>{if(t.length)return h(t,e)})}";
            out.replace(hld, strlen(kHLd), kHLdFix);
            fprintf(stderr, "[modpatch] loadable h() recursion fix applied in %s\n", url.c_str());
        }
    }
    /* EWEB_PLHREJ: diagnostic (log-only) on top of the h() fix above - name
     * WHICH resolver rejects and with what error, per recursion round. Each
     * resolver promise gets an extra observe-only then(onful,onrej) pair;
     * Promise.all still sees the original promise so settlement semantics
     * are unchanged. The reject message names the engine deficiency (e.g. a
     * missing global API thrown during chunk eval) that made one store
     * reject in the first place. */
    if(getenv("EWEB_PLHREJ") != nullptr) {
        const char* kHLdFixed =
            "function h(t,e){let r=[];for(;t.length;){let i=t.pop();r.push(i(e))}"
            "return Promise.all(r).then(()=>{if(t.length)return h(t,e)},"
            "()=>{if(t.length)return h(t,e)})}";
        size_t hrj = out.find(kHLdFixed);
        if(hrj != std::string::npos) {
            static const char* kHLdRej =
                "function h(t,e){globalThis.__plh=(globalThis.__plh|0)+1;"
                "let r=[],n=0;for(;t.length;){let i=t.pop();"
                "r.push((function(pp,k){try{pp.then(function(){"
                "console.error('[PLHREJ] ok r='+globalThis.__plh+' k='+k)},"
                "function(er){console.error('[PLHREJ] REJ r='+globalThis.__plh"
                "+' k='+k+' msg='+(er&&er.message?er.message:String(er))"
                "+' stk='+(er&&er.stack?String(er.stack).slice(0,400):'-'))})}"
                "catch(z){}return pp})(i(e),n++))}"
                "return Promise.all(r).then(()=>{if(t.length)return h(t,e)},"
                "()=>{if(t.length)return h(t,e)})}";
            out.replace(hrj, strlen(kHLdFixed), kHLdRej);
            fprintf(stderr, "[modpatch] PLHREJ instrumented in %s\n", url.c_str());
        }
        /* resolver registration site (same module): each loadable factory pushes
         * `e=>{for(let r of t)if(e.includes(r))return u()}` where t is the
         * loadable's webpack module-id list. Log registration order + ids so a
         * PLHREJ k-index (pop order = reverse of registration order) maps back to
         * the owning loadable's chunk/module ids. */
        const char* kReg =
            "t&&a.push(e=>{for(let r of t)if(e.includes(r))return u()})";
        size_t rg = out.find(kReg);
        if(rg != std::string::npos) {
            static const char* kRegIns =
                "t&&a.push(e=>{console.error('[PLHREG] n='+a.length"
                "+' ids='+t.join(','));for(let r of t)if(e.includes(r))return u()})";
            out.replace(rg, strlen(kReg), kRegIns);
            fprintf(stderr, "[modpatch] PLHREG instrumented in %s\n", url.c_str());
        }
        /* The round-1 resolver that REJECTS ("value is not a function!") owns
         * module 12655 (MarkdownStringToReact, next/dynamic): its loader is
         *   async()=>await Promise.all([r.e(8853),r.e(1661),r.e(4011),r.e(2655)])
         *     .then(r.bind(r,12655)).then(e=>e.MarkdownStringToReact)
         * Split the chain to tell whether a chunk-eval rejects or the module
         * 12655 factory itself throws during evaluation. */
        const char* kL55 =
            "Promise.all([r.e(8853),r.e(1661),r.e(4011),r.e(2655)])"
            ".then(r.bind(r,12655))";
        size_t l55 = out.find(kL55);
        if(l55 != std::string::npos) {
            static const char* kL55Ins =
                "Promise.all([r.e(8853),r.e(1661),r.e(4011),r.e(2655)])"
                ".then(function(){console.error('[L12655] chunks ok');"
                "var f=r.bind(r,12655);"
                "try{var M=f();console.error('[L12655] mod ok keys='"
                "+Object.keys(M).slice(0,6).join(','));return M}"
                "catch(q){console.error('[L12655] MOD THROW '+q);throw q}},"
                "function(er){console.error('[L12655] CHUNKS FAIL '+er);throw er})";
            out.replace(l55, strlen(kL55), kL55Ins);
            fprintf(stderr, "[modpatch] L12655 instrumented in %s\n", url.c_str());
        }
        /* module 12655 eval-time call sites that can throw "value is not
         * function!": `I=w()()` (w=a.n(87903); second invoke dies if the module's
         * default export is not a function) and `B=(0,m.l)()` (m=73613 unified
         * factory). Log callee types just before each invoke. */
        const char* kIW = "I=w()(),O=";
        size_t iw = out.find(kIW);
        if(iw != std::string::npos) {
            static const char* kIWIns =
                "I=(console.error('[M55] wtype='+typeof w),"
                "console.error('[M55] wdef='+typeof w()),"
                "w()()),O=";
            out.replace(iw, strlen(kIW), kIWIns);
            fprintf(stderr, "[modpatch] M55 w()() instrumented in %s\n", url.c_str());
        }
        const char* kBM = "B=(0,m.l)().use(";
        size_t bm = out.find(kBM);
        if(bm != std::string::npos) {
            static const char* kBMIns =
                "B=(console.error('[M55] ml='+typeof m.l),"
                "(0,m.l)()).use(";
            out.replace(bm, strlen(kBM), kBMIns);
            fprintf(stderr, "[modpatch] M55 m.l instrumented in %s\n", url.c_str());
        }
    }
    int nBnd = 0, nOv = 0;
    if(url.find("code-view") != std::string::npos) {
        const char* kMarker = "boundaryName:\"directory-content\"";
        size_t m = (getenv("EWEB_NO_RW1") != nullptr) ? std::string::npos : out.find(kMarker);
        while(m != std::string::npos) {
            size_t st = rfindJsxOpen(out, m);                   /* outer wrapper open */
            size_t en = out.find(",{})})", m);                /* inner close + wrapper close */
            size_t inner = (st != std::string::npos) ? findJsxOpen(out, st + 4) : std::string::npos;
            if(st != std::string::npos && en != std::string::npos &&
               inner != std::string::npos && inner > m && inner < en) {
                size_t inner_end = en + 4;   /* end of the inner element (",{})")  */
                size_t span_end  = en + 6;   /* end of the wrapper (",{})})") */
                std::string repl = out.substr(inner, inner_end - inner);
                out.replace(st, span_end - st, repl);
                nBnd++;
                m = out.find(kMarker, st + repl.size());
            } else {
                m = out.find(kMarker, m + 1);
            }
        }
        /* The file-table component element at the repo-root call site
         * (`(0,h.jsx)(XX,{overview:...})`) never renders as a direct child under
         * mario's reconciler; wrapping it in an inline function component adds
         * the fiber level that lets it mount (same effect the MODPATCH MUWRAP
         * probe had, without the logging). */
        const char* kOv = ",{overview:";
        size_t o = out.find(kOv);
        while(o != std::string::npos) {
            /* The component name is the identifier immediately preceding kOv;
             * only wrap when that identifier is the first argument of a jsx
             * factory call (so destructuring sites like `(0,x.c)(9),{overview:b}=e`
             * and host-element calls like `jsxs)("div",{...overview...}` deeper
             * in the props object are left alone). */
            size_t a0 = o;
            while(a0 > 0 && jsxIdentCh(out[a0 - 1])) a0--;
            bool ok = (a0 < o);
            size_t st = std::string::npos;
            if(ok) {
                size_t lim = (a0 > 24) ? a0 - 24 : 0;
                for(size_t q = a0; q-- > lim; ) {
                    if(isJsxOpenAt(out, q) && jsxOpenArgStart(out, q) == a0) { st = q; break; }
                }
                ok = (st != std::string::npos);
            }
            if(ok) {
                std::string name = out.substr(a0, o - a0);
                std::string repl = "function(w){return " + name + "(w)}";
                out.replace(a0, name.size(), repl);
                nOv++;
                o = out.find(kOv, a0 + repl.size());
                continue;
            }
            o = out.find(kOv, o + 1);
        }
        fprintf(stderr, "[modpatch] FUNC code-view boundaryRepl=%d overviewWrap=%d\n", nBnd, nOv);
    }
    /* app-foundation zone-info store: mario's useSyncExternalStore mis-drives
     * this subscribe/getSnapshot/getServerSnapshot trio (the SSR snapshot wins
     * and store changes never propagate), leaving dependent providers stuck on
     * their skeleton state. Calling the getSnapshot argument directly (the
     * "forced browser" path) matches what a standard browser observes once
     * hydrated. Matched structurally (3 short args after the call open) so
     * minified renames in bundle redeployments cannot silently disable it. */
    if(url.find("app-foundation") != std::string::npos) {
        int rw = ziForceBrowserRewrite(out);
        if(getenv("EWEB_MODPATCH_DBG") != nullptr)
            fprintf(stderr, "[modpatch] ZIRW url=%s rewrites=%d\n", url.c_str(), rw);
    }
    /* Functional patches for classic <script> bodies, applied unconditionally:
     * they paper over engine deficiencies that otherwise leave the page stuck
     * on its spinner/skeleton. */
    struct SPatch { const char* urlNeedle; const char* find; const char* repl; int all; };
    static const SPatch kSPatches[] = {
        /* Pinterest NOYIELD (same defect class as github's kPatches NOYIELD):
         * the inlined React 18 scheduler (module 175772 in www/_client-*.mjs)
         * yields whenever >=5ms elapsed since the deadline start; under mario a
         * single unit of work already exceeds that, so the post-lazy-chunk
         * retry render yields forever and the unauth page never commits past
         * its spinner fallback (hydration itself commits because it runs in
         * the synchronous lane). C is `unstable_shouldYield`; forcing false
         * lets each scheduler pass run to completion like github's NOYIELD. */
        {"www/_client-",
         "function C(){return!!y||!(t.unstable_now()-k<x)}",
         "function C(){return!1}", 0},
    };
    for(size_t pi = 0; pi < sizeof(kSPatches)/sizeof(kSPatches[0]); ++pi) {
        if(url.find(kSPatches[pi].urlNeedle) == std::string::npos) continue;
        size_t hit = out.find(kSPatches[pi].find);
        if(hit == std::string::npos) {
            fprintf(stderr, "[modpatch] SMISS needle#%zu in %s\n", pi, url.c_str());
            continue;
        }
        int nrep = 0;
        while(hit != std::string::npos) {
            out.replace(hit, strlen(kSPatches[pi].find), kSPatches[pi].repl);
            nrep++;
            if(!kSPatches[pi].all) break;
            hit = out.find(kSPatches[pi].find, hit + strlen(kSPatches[pi].repl));
        }
        fprintf(stderr, "[modpatch] SAPPLIED needle#%zu x%d to %s\n", pi, nrep, url.c_str());
    }
    return out;
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
    /* Parser-extracted script elements must exist before the first script runs,
     * including the pre-paint/document.write path. Self-locating bundles read
     * their own id/src synchronously during top-level evaluation. */
    if(m_jsNextScript == 0) jsMaterializeScriptStandIns();

    /* EWEB_DOMSIG: dump the hydration container's childNodes signature once,
     * before any bundle runs - this is the exact SSR DOM React will walk during
     * hydrateRoot, so it can be diffed against a real browser's capture. */
    if(getenv("EWEB_DOMSIG") != NULL && m_jsNextScript == 0) {
        static bool s_domsigDone = false;
        if(!s_domsigDone) {
            s_domsigDone = true;
            litehtml::document* d = jsActiveDoc();
            if(d != nullptr && d->root() != nullptr) {
                litehtml::element* cont = (litehtml::element*)d->root()->select_one("#__PWS_ROOT__");
                fprintf(stderr, "[domsig] === pre-hydration #__PWS_ROOT__ (cont=%d) ===\n",
                        cont != nullptr);
                ewebDumpDomSig(cont != nullptr ? cont : (litehtml::element*)d->root(), 0);
            }
        }
    }

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
        /* DIAG: record this script's base pc in the shared bytecode so a
         * MARIO_THROWDBG pc from an uncaught throw can be mapped onto the
         * MARIO_DUMPC disassembly of the same dumped script file. */
        PC jsBasePc = m_jsVm->bc.cindex;
        /* vm_load_run appends this script's bytecode after the previous one and
         * runs it; globals persist in vm->root across scripts, matching
         * separate <script> blocks that share one global scope. m_jsInScript
         * arms the VM step hook, which enforces the run budget and honours a
         * termination abort; window interaction stays live on its own thread
         * even through this pre-paint (document.write) script run. */
        /* SEGV checkpoint: a latent memory fault inside this bundle skips the
         * script instead of aborting the browser (see eweb_segv_checkpoint). */
        if(eweb_segv_checkpoint_tag(1) != 0) {
            fprintf(stderr, "[ewebview] js: script %d faulted (SIGSEGV) - skipping\n", (int)i);
            vm_terminate(m_jsVm);
            /* siglongjmp unwound every vm_run frame without running its restore, so
             * vm->pending_saves dangles at a dead stack frame; clear it (and the
             * abandoned transfer) or the next gc_vars dereferences wild memory. */
            vm_reset_pending(m_jsVm);
            m_jsVm->terminated = false;
            m_jsVm->abort_run = false;
            m_jsVm->propagating_err = nullptr;
            m_jsVm->call_depth = 0;
            m_jsInScript = false;
            m_jsEnterAt = 0;
            m_jsVm->dbg_tag = nullptr;
            m_jsCurScriptSrc = nullptr;
            m_jsCurScriptUrl.clear();
            continue;
        }
        jsVmEnter(true);
        m_jsCurScriptSrc = &src;
        m_jsCurScriptUrl = (i < m_jsScriptSrcs.size()) ? m_jsScriptSrcs[i] : std::string();
        jsSetModuleBase(true);
        {   /* Tag uncaught VM errors with the script index so a failing
             * minified bundle can be matched to its EWEB_DUMP_SCRIPTS file. */
            static char s_jsDbgTag[64];
            snprintf(s_jsDbgTag, sizeof(s_jsDbgTag), "script_%d", (int)i);
            m_jsVm->dbg_tag = s_jsDbgTag;
        }
        if(getenv("EWEB_SCRIPTDBG") != NULL)
            fprintf(stderr, "[ewebview] jsdbg: pre run script %d len=%u url=%.80s\n", (int)i,
                     (unsigned)src.size(), m_jsCurScriptUrl.c_str());
        if(getenv("MARIO_FLIGHTDBG") != NULL && src.find("__next_f") != std::string::npos)
            fprintf(stderr, "[flightdbg] script %d inline flight row:\n%s\n", (int)i, src.c_str());
        /* Same per-run state reset as the post-swap path (see above). */
        m_jsVm->terminated = false;
        m_jsVm->abort_run = false;
        m_jsVm->propagating_err = nullptr;
        m_jsVm->call_depth = 0;
        /* Module/defer scripts run after parsing ends in a real browser, so
         * they (and everything after them, up to DOMContentLoaded) observe
         * readyState=="interactive". Flip the bridge state monotonically when
         * the first such script runs; github's client-env module gates reading
         * its JSON island on that value. */
        bool deferredHere = (i < m_jsScriptDeferred.size() && m_jsScriptDeferred[i]);
        /* async externals likewise execute post-parse in a real browser, so
         * they observe readyState=="interactive"; flipping here is what lets
         * Next.js's flight bootstrap close its RSC stream before hydrateRoot
         * (see m_jsScriptAsync). Monotonic, like the defer flip. */
        bool asyncHere = (i < m_jsScriptAsync.size() && m_jsScriptAsync[i]);
        if(deferredHere || asyncHere)
            js_dom_set_parsing_complete(m_jsVm, true);
        if(!jsRunScriptBody(jsScriptModPatch(m_jsCurScriptUrl, src), deferredHere)) {
            EWEB_LOG("[ewebview] js: script %d failed to compile\n", (int)i);
            jsDumpScript(i, src, true);
        }
        else {
            jsDumpScript(i, src, false);
        }
        eweb_segv_disarm();
        m_jsVm->dbg_tag = nullptr;
        m_jsCurScriptSrc = nullptr;
        m_jsCurScriptUrl.clear();
        jsSetModuleBase(false);
        bool terminated = jsVmExit();
        if(getenv("EWEB_SCRIPTDBG") != NULL)
            fprintf(stderr, "[ewebview] jsdbg: pc_range script %d = [%u,%u)\n", (int)i,
                    (unsigned)jsBasePc, (unsigned)m_jsVm->bc.cindex);
        if(getenv("MARIO_PROMLEDGER") != NULL) {
            fprintf(stderr, "[promledger] --- after script %d ---\n", (int)i);
            mario_promise_ledger_dump(m_jsVm);
        }
        /* Same refund as the post-swap path: a watchdog-cut body must not eat
         * the pre-paint phase budget that decides when the first paint lands. */
        if(terminated && m_jsPrePaintAt != 0)
            m_jsPrePaintAt += (ticMs() - run_start);
        EWEB_LOG("[ewebview] js: script %d ran %u ms url=%s\n", (int)i,
            (uint32_t)(ticMs() - run_start),
            (i < m_jsScriptSrcs.size() && !m_jsScriptSrcs[i].empty())
                ? m_jsScriptSrcs[i].c_str() : "(inline)");
        EWEB_LOG("[ewebview] js: script %d pc_range=[%u,%u)\n", (int)i,
            (unsigned)jsBasePc, (unsigned)m_jsVm->bc.cindex);
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

    /* Defer custom-element connectedCallback reactions across the post-swap
     * queue: github's react-app upgrade awaits a code-view registration that a
     * LATER document-order script performs; upgrading inline mid-queue forces
     * the synchronous await spin / re-entrant script pump that corrupts the
     * suspended webpack factory frames. Drain once the queue is exhausted so
     * every registration exists before any lifecycle callback runs. */
    if(m_jsPostSwapAt != 0)
        js_dom_set_defer_upgrades(m_jsVm, true);
    if(m_jsNextScript >= m_jsScripts.size()) {
        js_dom_set_defer_upgrades(m_jsVm, false);
        js_dom_drain_deferred_upgrades(m_jsVm);
        /* EWEB_DOMSIG: post-hydration counterpart - once the script queue is
         * exhausted the client-rendered tree is final; dump its signature so a
         * collapse (opacity/height/position) can be seen. */
        if(getenv("EWEB_DOMSIG") != NULL) {
            static bool s_domsigPostHyd = false;
            if(!s_domsigPostHyd) {
                s_domsigPostHyd = true;
                litehtml::document* d = jsActiveDoc();
                if(d != nullptr && d->root() != nullptr) {
                    litehtml::element* cont = (litehtml::element*)d->root()->select_one("#__PWS_ROOT__");
                    fprintf(stderr, "[domsig] === POST-hydration #__PWS_ROOT__ (cont=%d) ===\n",
                            cont != nullptr);
                    ewebDumpDomSig(cont != nullptr ? cont : (litehtml::element*)d->root(), 0);
                }
            }
        }
    }

    /* EWEB_DOMSIG: post-swap is where the hydrateRoot bundle actually runs; dump
     * the hydration container's childNodes signature once, before the first
     * script, so the pre-hydration SSR DOM can be diffed against a browser. */
    if(getenv("EWEB_DOMSIG") != NULL && m_jsNextScript == 0) {
        static bool s_domsigPostDone = false;
        if(!s_domsigPostDone) {
            s_domsigPostDone = true;
            litehtml::document* d = jsActiveDoc();
            if(d != nullptr && d->root() != nullptr) {
                litehtml::element* cont = (litehtml::element*)d->root()->select_one("#__PWS_ROOT__");
                fprintf(stderr, "[domsig] === post-swap pre-hydration #__PWS_ROOT__ (cont=%d) ===\n",
                        cont != nullptr);
                ewebDumpDomSig(cont != nullptr ? cont : (litehtml::element*)d->root(), 0);
            }
        }
    }

    /* EWEB_HEADDUMP: dump the head child chain with full attribute tables at
     * the same post-swap pre-hydration instant. React 19 hydrates the head
     * singleton first; a node/attribute divergence vs raw SSR here (stand-in
     * scripts, engine-injected links, reorderings) desyncs the hydration
     * pointer and cascades into the body-level #418. Diff node-by-node. */
    if(getenv("EWEB_HEADDUMP") != NULL && m_jsNextScript == 0) {
        static bool s_headDone = false;
        if(!s_headDone) {
            s_headDone = true;
            litehtml::document* d = jsActiveDoc();
            if(d != nullptr && d->root() != nullptr) {
                litehtml::element* head = (litehtml::element*)d->root()->select_one("head");
                fprintf(stderr, "[headdump] === post-swap pre-hydration head=%p ===\n", (void*)head);
                ewebDumpHeadAttrs(head);
            }
        }
    }

    while(m_jsNextScript < m_jsScripts.size()) {
        size_t i = m_jsNextScript;
        /* Streaming-SSR early load: once the parser-extracted prefix recorded
         * at swap has run, fire DOMContentLoaded/load now even though dynamic
         * lazy chunks are still queued - a real browser does not let those
         * delay load, and gating on the 90s skeleton budget leaves scripts that
         * poll document.readyState=="complete" spinning the whole window
         * (pinterest.com), starving React's post-load work. The dynamic tail
         * keeps running afterwards, post-load. */
        if(m_jsStreamingSsr && m_jsPostSwapAt != 0 && !m_jsLoadFired &&
           m_jsInitialScriptCount > 0 && m_jsNextScript >= m_jsInitialScriptCount) {
            /* Gated pending parity validation: EWEB_EARLYLOAD=1 enables the
             * early load fire; default keeps the historical phase-completion
             * timing. */
            static int s_early = -1;
            if(s_early < 0) s_early = (getenv("EWEB_EARLYLOAD") != nullptr) ? 1 : 0;
            if(s_early) {
                jsRestoreNoJsFallback();
                jsFireLoadEvents();
            }
        }
        /* Post-swap phase wall clock: an ad-heavy portal carries dozens of
         * scripts, each able to burn a full kJsRunBudgetLiveMs, so without a
         * phase cap the engine never returns to serving input, a queued
         * navigation, or the blank-SPA notice decision - the page looks frozen
         * for tens of seconds. Past the budget stop STARTING new scripts (one
         * already in flight unwinds under its own per-run budget) and let the
         * phase complete: DOMContentLoaded/load fire and the page is usable,
         * the remaining ad scripts are simply dropped.
         * While the viewport still shows only the server-side skeleton the
         * tail of the queue IS the content bundle, so use the much larger
         * skeleton budget instead (see kJsPostSwapSkeletonBudgetMs).
         * LATCH: once the phase has seen a blank CSR shell, keep the skeleton
         * budget for the WHOLE tail. A client-rendered app paints its chrome
         * (nav/header) first and streams its main content from lazy chunks
         * still queued behind it, so re-probing "has real content" every
         * iteration and collapsing 90s->8s mid-phase instantly exhausts the
         * already-elapsed clock and drops the content-bearing tail
         * (pinterest.com: exhausted at script 60 of 74, dropping the lazy chunk
         * that registers the feed's module -> require miss -> the app dies
         * before it ever fetches the feed). Server-rendered portals have content
         * at swap, never latch, and keep the short live budget. */
        if(m_jsPostSwapAt != 0 && pageShowsSkeletonPlaceholder())
            m_postSwapSkelLatch = true;
        uint32_t postBudget = (m_postSwapSkelLatch ||
                               m_jsStreamingSsr ||
                               (m_jsBuildHasModules && pageHasSkeletonAny()))
                                  ? kJsPostSwapSkeletonBudgetMs
                                  : kJsPostSwapBudgetMs;
        if(m_jsPostSwapAt != 0 && (ticMs() - m_jsPostSwapAt) > postBudget) {
            /* Past the live budget. If the next unrun slot is a dynamically-
             * inserted <script> (m_jsScriptEls[i] != nullptr; parser-extracted
             * slots are nullptr - see the assign in queueBuild) it is an app-
             * driven lazy chunk the page is suspended on (webpack / React.lazy
             * park a promise on its load), NOT a droppable ad: a real browser
             * runs it, and dropping it permanently breaks the app. Latch the
             * skeleton budget so the content-bearing tail still runs, bounded
             * at kJsPostSwapSkeletonBudgetMs so a runaway cannot pin the engine.
             * pinterest.com: the SSR paints a nav (real content, so the blank-
             * shell latch above never fires) and the feed arrives only through
             * lazy chunks in this tail - one carries the module the batching
             * flush require()s, so cutting it at script 60 of 71 made require()
             * miss and the app died before it ever fetched the feed. A parser-
             * extracted tail (the ad-portal case the budget guards) still drops
             * at the live budget. */
            bool nextIsDynamic = (i < m_jsScriptEls.size() && m_jsScriptEls[i] != nullptr);
            if(nextIsDynamic && postBudget != kJsPostSwapSkeletonBudgetMs) {
                m_postSwapSkelLatch = true;
                postBudget = kJsPostSwapSkeletonBudgetMs;
            }
        }
        if(m_jsPostSwapAt != 0 && (ticMs() - m_jsPostSwapAt) > postBudget) {
            if(getenv("EWEB_SCRIPTDBG") != NULL)
                fprintf(stderr, "[ewebview] jsdbg: post budget %u ms exhausted at script %d of %d\n",
                        (unsigned)postBudget, (int)i, (int)m_jsScripts.size());
            EWEB_LOG("[ewebview] js: post-swap budget %u ms exhausted at script %d of %d - dropping the rest\n",
                (unsigned)postBudget, (int)i, (int)m_jsScripts.size());
            m_jsNextScript = m_jsScripts.size();
            m_jsPostSwapAt = 0;
            m_postSwapSkelLatch = false;
            break;
        }
        /* An external <script src> slot stays empty until its EWEB_TASK_SCRIPT
         * fetch lands (processResults fills m_jsScripts[i] and flips
         * m_jsScriptDone[i] to 1). Report "still running" so the engine keeps
         * BUILD_RUN_JS armed and re-enters us when the result wakes it - this
         * preserves document order without busy-spinning (the loop parks on a
         * 4ms tick; pushResult signals it the instant bytes arrive). A failed
         * fetch is still marked done with an empty body, so a 404 never wedges
         * the ordered run.
         * DEADLINE-SKIP (mirrors the pre-paint path above): a slot whose fetch
         * result URL never matches its src exactly (double-slash normalisation,
         * a redirect, a query-string rewrite) stays pending forever, and because
         * the run is strictly ordered it would wedge EVERY slot behind it - the
         * app bundle, lazy route chunks, all of it - so the page never paints.
         * Past the deadline skip the unresolvable slot exactly like a 404. */
        if(i < m_jsScriptDone.size() && !m_jsScriptDone[i]) {
            if(m_jsScriptWaitSince == 0) m_jsScriptWaitSince = ticMs();
            if(getenv("EWEB_SCRIPTDBG") != NULL &&
               (ticMs() - m_jsScriptWaitSince) % 2000 < 50)
                fprintf(stderr, "[ewebview] jsdbg: post wait script %d (%llu ms) src=%.80s\n",
                        (int)i, (unsigned long long)(ticMs() - m_jsScriptWaitSince),
                        (i < m_jsScriptSrcs.size() ? m_jsScriptSrcs[i].c_str() : "?"));
            if(ticMs() - m_jsScriptWaitSince < 10000) return true;
            EWEB_LOG("[ewebview] js: post-swap script %d fetch never resolved (src=%.80s) - skipping past deadline\n",
                (int)i, (i < m_jsScriptSrcs.size() ? m_jsScriptSrcs[i].c_str() : "?"));
            m_jsScriptDone[i] = 1;
            if(i < m_jsScripts.size()) m_jsScripts[i].clear();
            jsFireScriptElEvent(i, "error");   /* webpack d.l waits on onerror */
        }
        /* Refund the network wait to the post-swap phase clock: the budget
         * bounds engine work (each script already caps at its own run budget),
         * but on a live load the ordered queue parks on every external fetch,
         * so CDN latency - not script cost - consumed the phase window and the
         * content bundles at the tail were dropped (pinterest.com: budget
         * exhausted at script 39 of 61 while the viewport was still blank). */
        if(m_jsScriptWaitSince != 0) {
            uint64_t waited = ticMs() - m_jsScriptWaitSince;
            if(m_jsPostSwapAt != 0 && waited > 0)
                m_jsPostSwapAt += waited;
            m_jsScriptWaitSince = 0;
        }
        /* First script of this page: pre-materialise a stand-in <script src>
         * element for every queued external script so the DOM script list is
         * populated BEFORE any SDK reads it (see jsScriptStandInEl). */
        if(m_jsNextScript == 0) jsMaterializeScriptStandIns();
        m_jsNextScript++;
        /* Copy by value: this script may inject another (appendChild of a
         * <script>), which push_backs to m_jsScripts and can realloc - a
         * reference held across vm_load_run below would then dangle. */
        std::string src = m_jsScripts[i];
        if(src.empty()) {
            if(getenv("EWEB_SCRIPTDBG") != NULL)
                fprintf(stderr, "[ewebview] jsdbg: post skip script %d (empty body) src=%.80s done=%d\n",
                        (int)i, (i < m_jsScriptSrcs.size() ? m_jsScriptSrcs[i].c_str() : "?"),
                        (i < m_jsScriptDone.size() ? (int)m_jsScriptDone[i] : -1));
            jsFireScriptElEvent(i, "error"); continue;
        }
        if(jsIsRunaway(src)) {
            EWEB_LOG("[ewebview] js: script %d skipped (runaway body, %u bytes)\n",
                (int)i, (unsigned)src.size());
            jsFireScriptElEvent(i, "error");
            continue;
        }
        uint64_t run_start = ticMs();
        /* DIAG: base pc of this script in the shared bytecode (pairs with the
         * pc_range log below) so a MARIO_THROWDBG frame pc can be mapped onto
         * the MARIO_DUMPC disassembly of the same dumped script file. */
        PC jsBasePc = m_jsVm->bc.cindex;
        /* Bracket vm_load_run so the DOM-bridge mutation callbacks push the
         * page to the screen mid-script (jsMarkLayoutDirty -> jsProgressiveFlush
         * -> engineRenderFrame): a long test script then shows its results as
         * they are produced. m_jsInScript arms the step hook (run budget +
         * abort). Window interaction is unaffected - it runs on the UI thread. */
        m_jsProgressiveActive = true;
        /* SEGV checkpoint: a latent memory fault inside this bundle skips the
         * script instead of aborting the browser (see eweb_segv_checkpoint). */
        if(eweb_segv_checkpoint_tag(2) != 0) {
            fprintf(stderr, "[ewebview] js: post script %d faulted (SIGSEGV) - skipping\n", (int)i);
            vm_terminate(m_jsVm);
            /* siglongjmp unwound every vm_run frame without running its restore, so
             * vm->pending_saves dangles at a dead stack frame; clear it (and the
             * abandoned transfer) or the next gc_vars dereferences wild memory. */
            vm_reset_pending(m_jsVm);
            m_jsVm->terminated = false;
            m_jsVm->abort_run = false;
            m_jsVm->propagating_err = nullptr;
            m_jsVm->call_depth = 0;
            m_jsInScript = false;
            m_jsEnterAt = 0;
            m_jsVm->dbg_tag = nullptr;
            m_jsCurScriptSrc = nullptr;
            m_jsCurScriptUrl.clear();
            m_jsProgressiveActive = false;
            continue;
        }
        jsVmEnter(true);
        m_jsCurScriptSrc = &src;
        m_jsCurScriptUrl = (i < m_jsScriptSrcs.size()) ? m_jsScriptSrcs[i] : std::string();
        jsSetModuleBase(true);
        m_jsLastFlushAt = run_start;
        {
            static char s_jsDbgTag[64];
            snprintf(s_jsDbgTag, sizeof(s_jsDbgTag), "script_%d", (int)i);
            m_jsVm->dbg_tag = s_jsDbgTag;
        }
        if(getenv("EWEB_SCRIPTDBG") != NULL)
            fprintf(stderr, "[ewebview] jsdbg: post run script %d len=%u url=%.80s\n", (int)i,
                     (unsigned)src.size(), m_jsCurScriptUrl.c_str());
        if(getenv("MARIO_FLIGHTDBG") != NULL && src.find("__next_f") != std::string::npos)
            fprintf(stderr, "[flightdbg] post script %d inline flight row:\n%s\n", (int)i, src.c_str());
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
        /* Same monotonic readyState flip as the pre-paint path (see above). */
        bool deferredHere = (i < m_jsScriptDeferred.size() && m_jsScriptDeferred[i]);
        /* async externals likewise execute post-parse in a real browser, so
         * they observe readyState=="interactive"; flipping here is what lets
         * Next.js's flight bootstrap close its RSC stream before hydrateRoot
         * (see m_jsScriptAsync). Monotonic, like the defer flip. */
        bool asyncHere = (i < m_jsScriptAsync.size() && m_jsScriptAsync[i]);
        if(deferredHere || asyncHere)
            js_dom_set_parsing_complete(m_jsVm, true);
        if(!jsRunScriptBody(jsScriptModPatch(m_jsCurScriptUrl, src), deferredHere)) {
            EWEB_LOG("[ewebview] js: script %d failed to compile\n", (int)i);
            jsDumpScript(i, src, true);
        }
        else {
            jsDumpScript(i, src, false);
        }
        eweb_segv_disarm();
        m_jsVm->dbg_tag = nullptr;
        m_jsCurScriptSrc = nullptr;
        m_jsCurScriptUrl.clear();
        jsSetModuleBase(false);
        bool terminated = jsVmExit();
        if(getenv("EWEB_SCRIPTDBG") != NULL)
            fprintf(stderr, "[ewebview] jsdbg: pc_range script %d = [%u,%u)\n", (int)i,
                    (unsigned)jsBasePc, (unsigned)m_jsVm->bc.cindex);
        /* A watchdog-cut body's wall time is waste, not page work: refund it to
         * the phase budget so the scripts BEHIND a runaway SDK still get their
         * window (taobao's traceSDK burns a whole cut ahead of the React
         * bundles that would render the page). A body that finished on its own
         * keeps its cost charged, so a portal of slow-but-legit scripts still
         * hits the phase cap. */
        if(terminated && m_jsPostSwapAt != 0)
            m_jsPostSwapAt += (ticMs() - run_start);
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
            m_jsScriptSrcs.push_back(i < m_jsScriptSrcs.size()
                                        ? m_jsScriptSrcs[i] : std::string());
            m_jsScriptIds.push_back(i < m_jsScriptIds.size()
                                       ? m_jsScriptIds[i] : std::string());
            m_jsScriptDeferred.push_back(i < m_jsScriptDeferred.size()
                                            ? m_jsScriptDeferred[i] : false);
            m_jsScriptDone.push_back(1);
            m_jsScriptEls.push_back(nullptr);   /* keep the vectors aligned */
            for(auto it = m_jsRunawaySrcs.begin(); it != m_jsRunawaySrcs.end(); ++it) {
                if(*it == src) { m_jsRunawaySrcs.erase(it); break; }
            }
            EWEB_LOG("[ewebview] js: script %d requeued at tail (watchdog cut)\n", (int)i);
        }
        EWEB_LOG("[ewebview] js: script %d ran %u ms (post-swap) url=%s\n",
            (int)i, (uint32_t)(ticMs() - run_start),
            (i < m_jsScriptSrcs.size() && !m_jsScriptSrcs[i].empty())
                ? m_jsScriptSrcs[i].c_str() : "(inline)");
        /* TEMP DIAGNOSTIC (EWEB_SCRIPTRANDBG): release builds compile EWEB_LOG
         * out; this env-gated line keeps entry-script execution (duration,
         * watchdog cut) visible while chasing the github code-view stall. */
        if(getenv("EWEB_SCRIPTRANDBG") != nullptr) {
            fprintf(stderr, "[scriptran] script %d ran %u ms terminated=%d url=%s\n",
                (int)i, (uint32_t)(ticMs() - run_start), terminated ? 1 : 0,
                (i < m_jsScriptSrcs.size() && !m_jsScriptSrcs[i].empty())
                    ? m_jsScriptSrcs[i].c_str() : "(inline)");
        }
        EWEB_LOG("[ewebview] js: script %d pc_range=[%u,%u)\n", (int)i,
            (unsigned)jsBasePc, (unsigned)m_jsVm->bc.cindex);
        if(getenv("MARIO_PROMLEDGER") != NULL) {
            fprintf(stderr, "[promledger] --- after script %d (post-swap) ---\n", (int)i);
            mario_promise_ledger_dump(m_jsVm);
        }
        /* The body ran (or was cut after running): fire the element's load
         * event so webpack's d.l chunk loader resolves its promise. A cut or
         * faulted body reports error instead - the loader must settle either
         * way or the chunk pipeline wedges. */
        jsFireScriptElEvent(i, terminated ? "error" : "load");
        /* Paint what this script produced before the next one runs. */
        jsProgressiveFlush(true);
        /* DIAGNOSTIC HOOK (EWEB_INJECT_JS=<file>): run the file's body in the
         * page's global scope right after an early anchor bundle executes, so
         * its XHR/fetch/error hooks are installed BEFORE the app-entry bundle
         * (main.js) runs and fires the route-loader data requests - hooking
         * after the entry races those requests and misses them. The anchor is
         * matched by "react-dom" in the src (React is loaded, XMLHttpRequest
         * and fetch exist, the webpack app chunks have not run yet). Fires once
         * per page. */
        if(!m_jsInjectDone && i < m_jsScriptSrcs.size()) {
            /* TEMP: anchor substring overridable so sites whose react-dom chunk
             * is hash-named (rokid: fd9d1056-*.js) can still be hooked. */
            const char* anchor = getenv("EWEB_INJECT_ANCHOR");
            if(anchor == nullptr || anchor[0] == 0) anchor = "react-dom";
            if(m_jsScriptSrcs[i].find(anchor) != std::string::npos) {
            const char* injPath = getenv("EWEB_INJECT_JS");
            if(injPath != nullptr && injPath[0] != 0) {
                m_jsInjectDone = true;
                std::string body;
                FILE* f = fopen(injPath, "rb");
                if(f != nullptr) {
                    char buf[4096];
                    size_t n;
                    while((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
                    fclose(f);
                }
                if(!body.empty()) {
                    EWEB_LOG("[ewebview] js: running EWEB_INJECT_JS=%s (%u bytes) after entry script %d\n",
                        injPath, (unsigned)body.size(), (int)i);
                    m_jsVm->terminated = false;
                    m_jsVm->abort_run = false;
                    m_jsVm->propagating_err = nullptr;
                    m_jsVm->call_depth = 0;
                    jsVmEnter();
                    if(!vm_load_run(m_jsVm, body.c_str()))
                        EWEB_LOG("[ewebview] js: inject body failed to compile\n");
                    jsVmExit();
                    jsProgressiveFlush(true);
                }
            }
            }
        }
        break;
    }
    /* Queue exhausted in this call: run the deferred connectedCallback
     * reactions now (same tick as the last script) so lifecycle callbacks see
     * every registration the queue performed. */
    if(m_jsNextScript >= m_jsScripts.size()) {
        js_dom_set_defer_upgrades(m_jsVm, false);
        js_dom_drain_deferred_upgrades(m_jsVm);
    }
    return m_jsNextScript < m_jsScripts.size();
}

/* Fire "load"/"error" on the dynamic <script> element behind slot i (see
 * m_jsScriptEls). webpack's d.l loader parks the chunk promise on the
 * element's onload/onerror property handlers; without this dispatch a
 * dynamically loaded chunk never settles its promise and everything awaiting
 * it (Next.js RSC module deps -> hydration -> next/script injection) stalls.
 * The element handle may have died since the insert (removeChild + page
 * teardown), so validate it before dispatching. */
void EWebEngine::jsFireScriptElEvent(size_t i, const char* type)
{
    if(m_jsVm == nullptr || i >= m_jsScriptEls.size()) return;
    void* el = m_jsScriptEls[i];
    m_jsScriptEls[i] = nullptr;   /* fire at most once per element */
    if(el == nullptr || !jsElIsLive(this, el)) return;
    jsVmEnter();
    js_event_dispatch_simple(m_jsVm, el, type, false);
    jsVmExit();
}

void EWebEngine::jsNoteDynamicStylesheet(const std::string& url, void* link_el)
{
    if(url.empty() || link_el == nullptr) return;
    if(getenv("EWEB_MODPATCH_DBG") != nullptr)
        fprintf(stderr, "[csslink] NOTE url=%s\n", url.c_str());
    m_jsLinkEls.push_back(std::make_pair(url, link_el));
}

void EWebEngine::jsFireLinkElEvent(const std::string& url, const char* type)
{
    if(m_jsVm == nullptr || url.empty()) return;
    void* el = nullptr;
    for(size_t i = 0; i < m_jsLinkEls.size(); ++i) {
        if(m_jsLinkEls[i].first == url) {
            el = m_jsLinkEls[i].second;
            m_jsLinkEls.erase(m_jsLinkEls.begin() + (long)i);
            break;
        }
    }
    if(getenv("EWEB_MODPATCH_DBG") != nullptr) {
        fprintf(stderr, "[csslink] FIRE %s url=%s found=%d pending=%zu\n",
                type, url.c_str(), el != nullptr ? 1 : 0, m_jsLinkEls.size());
        if(el == nullptr)
            for(size_t i = 0; i < m_jsLinkEls.size(); ++i)
                fprintf(stderr, "[csslink]   pending[%zu]=%s\n", i, m_jsLinkEls[i].first.c_str());
    }
    if(el == nullptr || !jsElIsLive(this, el)) return;
    EWEB_LOG("[ewebview] jsdbg: fire link %s url=%.60s\n", type, url.c_str());
    jsVmEnter();
    js_event_dispatch_simple(m_jsVm, el, type, false);
    jsVmExit();
}

void EWebEngine::jsNoteImageEl(const std::string& url, void* img_el)
{
    if(url.empty() || img_el == nullptr) return;
    m_jsImgEls.push_back(std::make_pair(url, img_el));
}

void EWebEngine::jsFireImgElEvent(const std::string& url, const char* type)
{
    if(m_jsVm == nullptr || url.empty()) return;
    /* Unlike a <link> chunk (one element per sheet), several <img> nodes can
     * await the SAME url (React preloads an image with new Image() and then
     * appends a real <img> carrying the same src). Collect every waiter and
     * fire them all, then drop the entries. */
    void* els[16];
    int n = 0;
    for(size_t i = 0; i < m_jsImgEls.size() && n < 16; ) {
        if(m_jsImgEls[i].first == url) {
            els[n++] = m_jsImgEls[i].second;
            m_jsImgEls.erase(m_jsImgEls.begin() + (long)i);
        } else {
            ++i;
        }
    }
    for(int k = 0; k < n; ++k) {
        void* el = els[k];
        if(el == nullptr || !jsElIsLive(this, el)) continue;
        jsVmEnter();
        js_event_dispatch_simple(m_jsVm, el, type, false);
        jsVmExit();
    }
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
    /* Honour a non-JS type= exactly like the parser-side extract_scripts() does.
     * React's streaming reveal ($RV) MOVES data-island scripts such as
     * <script id="__PWS_INITIAL_PROPS__" type="application/json"> into the tree
     * via insertBefore; without this guard the JSON body was queued as JS and
     * died with a compile error (`{` block + `"key"` + unexpected `:`). Data
     * islands are read through textContent, never executed. type="module" keeps
     * its existing behaviour; a missing/empty type defaults to JS. */
    {
        const char* type_attr = sc->get_attr("type", nullptr);
        if(type_attr != nullptr && type_attr[0] != 0) {
            std::string tv = type_attr;
            for(char& ch : tv) ch = (char)::tolower((unsigned char)ch);
            bool js_mime = (tv.find("javascript") != std::string::npos ||
                            tv == "text/ecmascript" ||
                            tv == "application/ecmascript");
            if(!js_mime && tv != "module") {
                EWEB_LOG("[ewebview] dynScript skip non-JS type=%s\n", type_attr);
                return;
            }
        }
    }
    /* TEMP DIAGNOSTIC (taobao): webpack loads route chunks by splicing
     * <script src> into <head>; log every such insert so a missing async chunk
     * (e.g. fdabd03d.js) is visible. Remove with the other temp probes. */
    if(getenv("EWEB_SCRIPTDBG") != nullptr) {
        const char* sa = sc->get_attr("src", nullptr);
        fprintf(stderr, "[ewebview] jsdbg: dynScript insert src=%s\n",
                (sa != nullptr && sa[0] != 0) ? sa : "(inline)");
    }

    std::string body;
    std::string srcabs;
    const char* src_attr = sc->get_attr("src", nullptr);
    EWEB_LOG("[ewebview] dynScript src_attr=%s docurl=%s\n",
        src_attr != nullptr ? src_attr : "(null)", jsDocumentUrl().c_str());
    if(src_attr != nullptr && src_attr[0] != 0) {
        /* External: resolve against the document URL and fetch it. An
         * unresolvable src is dropped so it can never block the ordered run -
         * but the element still gets its error event: webpack's d.l parks the
         * chunk promise on script.onerror and a silent drop wedges it. */
        srcabs = EWebContainer::getFullURL(&m_port, src_attr, jsDocumentUrl());
        if(srcabs.empty()) {
            js_event_dispatch_simple(m_jsVm, script_el, "error", false);
            return;
        }
    } else {
        /* Inline: the body is the element's text; skip a blank one. */
        sc->get_text(body);
        if(getenv("EWEB_SCRIPTDBG") != nullptr)
            fprintf(stderr, "[ewebview] jsdbg: dynScript inline body.len=%u head=%.60s\n",
                    (unsigned)body.size(), body.c_str());
        if(getenv("MARIO_FLIGHTDBG") != nullptr && body.find("__next_f") != std::string::npos)
            fprintf(stderr, "[flightdbg] dyn inline flight row:\n%s\n", body.c_str());
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
        m_jsScriptIds.push_back(std::string());
        /* A dynamically inserted script executes as soon as it loads; by then
         * parsing is over unless a defer/module script has run yet, so the
         * readyState flag stays untouched here (matches the classic case). */
        m_jsScriptDeferred.push_back(false);
        m_jsScriptDone.push_back(external ? 0 : 1);
        /* Remember the element so the ordered run can fire load/error on it
         * (see m_jsScriptEls). Only part 0 of a split combo owns the element;
         * its load fires when that first part runs. */
        m_jsScriptEls.push_back(k == 0 ? script_el : nullptr);
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
        m_postSwapSkelLatch = false;   /* re-latch from the probe if still blank */
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
    m_jsScriptEls.clear();
    m_jsCurScriptEl = nullptr;
    m_jsCurScriptElUrl.clear();
    EWEB_LOG("[ewebview] js: document.write spliced %d byte(s), reparse #%d\n",
        (int)written.size(), m_jsReparseCount);
    return true;
}

/* ==================================================================
 * VM run watchdog (see kJsRunBudgetMs / kJsRunAbortMax in EWebInternal.h)
 * ================================================================== */

void EWebEngine::jsVmEnter(bool appScript)
{
    /* Wall-clock anchor for the step-hook budget. m_jsInScript marks that a VM
     * run is in flight on the engine thread; a navigation requested from inside
     * it is deferred until it unwinds (see jsRunPendingNavigation). The abort
     * generation is snapshotted so jsOnVmStep can tell an abort raised DURING
     * this run apart from a stale flag left by a STOP consumed while idle. */
    m_jsEnterAt = ticMs();
    /* A/B gate (EWEB_FORCESKEL): grant EVERY non-prepaint run the long skeleton
     * budget regardless of build-type detection, to test whether a React SPA
     * whose render/recovery task exceeds kJsRunBudgetLiveMs (apple /mac) is being
     * watchdog-cut mid-render. Cached once. */
    static int s_forceSkelBudget = -1;
    if(s_forceSkelBudget < 0) s_forceSkelBudget = (getenv("EWEB_FORCESKEL") != nullptr) ? 1 : 0;
    /* The pre-paint phase answers to its own wall clock, so a single run may
     * keep the (larger) legacy budget; every run against a live page gets the
     * short one, because the engine thread cannot serve input while it lasts.
     * While only the server-side skeleton paints, the app entry that replaces
     * it is one long top-level body, so it gets the skeleton run budget instead
     * (see kJsRunBudgetSkeletonMs). A module-script page that still shows ANY
     * skeleton placeholder is in the same boat even though real content paints
     * around it (github.com's file tree): its hydration bundles are single
     * multi-second bodies, and cutting them at the live budget aborts the boot
     * three times and drops the page's JS entirely.
     * A module build paints its SSR shell IMMEDIATELY (pinterest.com), so the
     * skeleton probes are false from the start even though the hydration bundle
     * is still one multi-second body. Critically, a module React app runs its
     * ENTIRE render/commit work loop inside scheduler (MessageChannel/setTimeout)
     * task callbacks, NOT in the top-level appScript body: those task callbacks
     * (appScript=false, run via jsPollTimers) are where renderRootSync lives, and
     * a single synchronous render measured 26 s on pinterest.com's fiber tree.
     * Cutting such a task at the short live budget mid-render leaves React's
     * module-level executionContext stuck at RenderContext (sN=2): every later
     * scheduling flush then takes the deferred branch and performConcurrentWork
     * throws error #327 on entry, so the retry render scheduled after the lazy
     * chunks load never runs and the page stays permanently blank. So on module
     * builds EVERY run - top-level body AND task callback - keeps the long
     * skeleton budget. The budget is a CEILING, not a fixed cost: short timer
     * callbacks still finish in milliseconds, so raising it only lets genuine
     * multi-second renders complete while kJsRunAbortMax still drops a page
     * whose tasks truly run away. Classic (non-module) pages are unaffected and
     * keep the short live budget for every run, preserving the ad-portal
     * input-responsiveness guarantee.
     * A streaming-SSR build (m_jsStreamingSsr: hidden <div id="S:n"> segments +
     * trailing $RC("B:n","S:n") reveals) is in the SAME boat as a module build
     * even when its app scripts are classic: pinterest.com ships its React
     * bundles as async classic webpack-JSONP chunks ((self.modernJsonp=...).push)
     * with NO type="module", so m_jsBuildHasModules is false, yet its
     * renderRootSync still runs entirely inside MessageChannel/setTimeout task
     * callbacks (appScript=false) with no shouldYield in the sync work loop. A
     * single synchronous render of its fiber tree far exceeds the short live
     * budget; cutting that task mid-render leaves React's module-level
     * executionContext stuck at RenderContext (sN=2), after which every later
     * scheduling flush takes the deferred branch, the CSR shell never fills and
     * the stalled context dereferences a torn workInProgress -> SIGBUS. So a
     * streaming-SSR build grants EVERY run - top-level body AND task callback -
     * the long skeleton budget, exactly like a module build. Confirmed by A/B:
     * with the budget lifted the same page goes els 164->869, docH 844->4254,
     * sN=2 count 0, EXIT 0. This is the per-run analogue of the post-swap PHASE
     * budget already keyed on m_jsStreamingSsr (see queueBuild).
     * A latched CSR-shell post-swap phase (m_postSwapSkelLatch) is the classic-
     * script analogue of the module case: its content-bearing tail is still
     * streaming in after the chrome painted, so top-level script runs (appScript)
     * keep the long budget and are not watchdog-cut mid-bundle, while timer
     * callbacks / event handlers stay on the short one for input. Gated on
     * m_jsPostSwapRun so the latch never leaks into IDLE-time runs. */
    m_jsRunDeadline = m_jsEnterAt +
        ((m_jsPrePaintAt != 0) ? kJsRunBudgetMs
                               : (jsSkeletonProbeCached() ||
                                  (m_jsPostSwapRun && m_postSwapSkelLatch && appScript) ||
                                  m_jsBuildHasModules ||
                                  m_jsStreamingSsr ||
                                  s_forceSkelBudget)
                                      ? kJsRunBudgetSkeletonMs
                                      : kJsRunBudgetLiveMs);
    m_jsEnterGen = m_buildAbortGen;
    m_jsAbortPrePaint = false;
    m_jsInScript = true;
    /* An async event-loop callback (appScript=false: timer poll, MessageChannel
     * drain, fetch settle, event dispatch) only ever runs after the parser
     * finished in a real browser, so it observes readyState=="interactive".
     * Sync app-script bodies keep the build-phase "loading" (Next.js's inline
     * flight bootstrap defers its stream-close to DOMContentLoaded on that);
     * pinterest.com's MessageChannel-scheduled React render needs the post-parse
     * value or its dehydrated-Suspense client-render gate stays shut and the CSR
     * shell never fills. See js_dom_set_async_cb / native_document_get_readyState. */
    if(m_jsVm != nullptr)
        js_dom_set_async_cb(m_jsVm, !appScript);
}

bool EWebEngine::jsVmExit()
{
    m_jsInScript = false;
    m_jsEnterAt = 0;
    /* Clear the async-callback readyState flip set in jsVmEnter so a following
     * sync script body reports the build-phase value again (see jsVmEnter). */
    if(m_jsVm != nullptr)
        js_dom_set_async_cb(m_jsVm, false);
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

/* Await spin tick (mario vm->on_await_pending). mario's __await() has no
 * suspension: without this hook an await on a promise that settles later
 * (timer / MessageChannel microtask) yields undefined and the async function
 * ploughs on with garbage - which is exactly how ice.js's runApp "completed"
 * yet never reached React's render. Here we let the awaited promise settle by
 * pumping the DOM timer table (setTimeout/setInterval AND queued
 * MessageChannel posts, which ride it at 0 ms). When nothing is due but timers
 * are still armed we sleep ~1 ms so delayed callbacks get real wall-clock
 * time; when the table is empty the promise can never settle by timers alone,
 * so the tick falls back to advancing the post-swap script queue (see
 * jsPumpAwaitScript) - mario's vm_run reloads its cached code pointer after
 * every dispatch, which is what makes compiling a further script from inside
 * the spin safe. When the queue is drained too, the tick returns 0 so the
 * await degrades to undefined as before. Per-promise wall-clock cap keeps a
 * genuinely stuck await from hanging the run. */
/* Await-pump script advance: evaluate the next queued post-swap script whose
 * body is already fetched, IN DOCUMENT ORDER, from inside an await spin. Some
 * promises only settle in a LATER script's top level - github.com's react-app
 * registry is filled by the code-view chunk (script 8) while the element's
 * connectedCallback awaits it during behaviors (script 4); with no true
 * suspension the await would degrade to undefined and hydration never starts.
 * Consuming the queue head keeps document order intact: the outer post-swap
 * loop re-reads m_jsNextScript when its current iteration finishes, so a
 * script the pump ran is never run twice. The nested run deliberately skips
 * jsVmEnter/jsVmExit (their deadline/generation slots belong to the OUTER
 * frame, whose watchdog keeps bounding all of this work) and restores the
 * outer frame's per-script context and VM residue on the way out. */
int EWebEngine::jsPumpAwaitScript()
{
    if(m_jsVm == nullptr || m_jsPostSwapAt == 0) return 0;
    if(m_jsNextScript >= m_jsScripts.size()) return 0;
    size_t i = m_jsNextScript;
    if(i >= m_jsScriptDone.size() || !m_jsScriptDone[i]) return 0;
    std::string src = m_jsScripts[i];
    m_jsNextScript++;
    if(src.empty()) { jsFireScriptElEvent(i, "error"); return 1; }
    if(getenv("EWEB_SCRIPTDBG") != NULL)
        fprintf(stderr, "[ewebview] jsdbg: await-pump run script %d len=%u url=%.80s\n",
                (int)i, (unsigned)src.size(),
                (i < m_jsScriptSrcs.size() ? m_jsScriptSrcs[i].c_str() : "?"));
    /* Outer frame context to restore afterwards. */
    const std::string* saveSrc  = m_jsCurScriptSrc;
    std::string        saveUrl  = m_jsCurScriptUrl;
    bool               saveProg = m_jsProgressiveActive;
    const char*        saveTag  = m_jsVm->dbg_tag;
    int                saveDepth = m_jsVm->call_depth;
    bool               saveTerm  = m_jsVm->terminated;
    bool               saveAbort = m_jsVm->abort_run;
    var_t*             saveErr   = m_jsVm->propagating_err;
    /* Interpreter resume state. The pumped body is a nested vm_load_run/vm_run
     * on THIS vm: it leaves vm->pc at the pumped script's last instruction, so
     * the suspended outer frame (the async function whose spin we are inside)
     * must get its own pc back - a foreign pc made it decode the pumped
     * script's tail as its own bytecode. The scope/value stacks are handled by
     * vm_pump_suspend/vm_pump_resume below, which detach the suspended frames
     * entirely so the pumped chunk's top-level bare names can never resolve
     * into (and clobber) the suspended webpack factory's single-letter locals. */
    auto               savePc   = m_jsVm->pc;
    auto               saveScopeTop = m_jsVm->scope_stack_top;
    auto               saveStackTop = m_jsVm->stack_top;

    static char s_pumpTag[64];
    snprintf(s_pumpTag, sizeof(s_pumpTag), "pump_%d", (int)i);
    m_jsVm->dbg_tag = s_pumpTag;
    m_jsCurScriptSrc = &src;
    m_jsCurScriptUrl = (i < m_jsScriptSrcs.size()) ? m_jsScriptSrcs[i] : std::string();
    jsSetModuleBase(true);
    bool deferredHere = (i < m_jsScriptDeferred.size() && m_jsScriptDeferred[i]);
    if(deferredHere)
        js_dom_set_parsing_complete(m_jsVm, true);
    /* Detach the ENTIRE live scope/value stack (the suspended webpack factory
     * frames, incl. De(e,t,r)'s single-letter locals) into the gc-rooted pump
     * anchor, so the pumped chunk's top level runs on a clean stack whose bare
     * names can only reach the global object - never resolve into (and ASSIGN
     * over) the parked frame's `r`/`e`/`t`. Without this the pumped body shares
     * the suspended frames' scope chain and clobbers the require param, which
     * surfaces later as "can not find function 'r' on object{...}". */
    vm_pump_suspend(m_jsVm);
    if(!jsRunScriptBody(jsScriptModPatch(m_jsCurScriptUrl, src), deferredHere) && getenv("EWEB_SCRIPTDBG") != NULL)
        fprintf(stderr, "[ewebview] jsdbg: await-pump script %d failed\n", (int)i);
    vm_pump_resume(m_jsVm);
    jsSetModuleBase(false);

    m_jsCurScriptSrc    = saveSrc;
    m_jsCurScriptUrl    = saveUrl;
    m_jsProgressiveActive = saveProg;
    m_jsVm->dbg_tag     = saveTag;
    m_jsVm->call_depth  = saveDepth;
    m_jsVm->terminated  = saveTerm;
    m_jsVm->abort_run   = saveAbort;
    m_jsVm->propagating_err = saveErr;
    m_jsVm->pc          = savePc;
    m_jsVm->gc.gc_defer--;
    return 1;
}

int EWebEngine::jsAwaitPendingTick(struct st_vm* vm, var_t* promise)
{
    if(vm == nullptr) return 0;
    EWebEngine* self = (EWebEngine*)vm->on_step_data;
    if(self == nullptr) return 0;
    static var_t* s_last = nullptr;
    static uint64_t s_deadline = 0;
    if(promise != s_last) { s_last = promise; s_deadline = self->ticMs() + 3000; }
    if(self->ticMs() >= s_deadline) return 0;
    int fired = js_dom_poll_timers(vm, self->ticMs());
    if(fired > 0) return 1;
    if(js_dom_has_pending_timers(vm)) {
        usleep(1000);
        return 1;
    }
    /* No timer can settle this promise: try the next queued script instead
     * (see jsPumpAwaitScript). Returns 0 once the queue is drained, letting
     * the await degrade to undefined exactly as before. */
    return self->jsPumpAwaitScript();
}

void EWebEngine::jsOnVmStep(struct st_vm* vm)
{
    if(vm == nullptr || vm->terminated)
        return;
    /* DIAG (temp, taobao): a body that burns the whole live run budget without
     * touching the DOM is spinning in pure JS; sampling vm->pc at a fixed
     * interval while EWEB_SPINDBG is set exposes the hot loop's pc range so it
     * can be matched against a MARIO_DUMPC disassembly of the dumped script.
     * Remove with the other temp probes. */
    static int s_spin = -1;
    if(s_spin < 0) s_spin = (getenv("EWEB_SPINDBG") != nullptr) ? 1 : 0;
    {
        static uint64_t s_spin_last = 0;
        if(s_spin) {
            uint64_t t = ticMs();
            if(t - s_spin_last >= 400) {
                s_spin_last = t;
                fprintf(stderr, "[spindbg] t=%llu pc=%u depth=%d tag=%s\n",
                        (unsigned long long)t, (unsigned)vm->pc,
                        (int)vm->call_depth,
                        vm->dbg_tag != nullptr ? vm->dbg_tag : "-");
            }
        }
    }
    /* A termination request (stop / back / reload / exit) bumps m_buildAbortGen
     * from the UI thread; if the generation moved past this run's jsVmEnter
     * snapshot the abort arrived DURING this run, so unwind at once instead of
     * running to the watchdog budget. An unchanged generation means any raised
     * m_buildAbort is stale (a STOP consumed while the page sat idle) and this
     * run belongs to the still-visible page: let it proceed. */
    if(m_buildAbortGen != m_jsEnterGen) {
        if(getenv("MARIO_RUNEXIT") != nullptr)
            fprintf(stderr, "[RUNEXIT] stephook GEN terminate pc=%u depth=%d gen %u!=%u\n",
                 (unsigned)vm->pc, (int)vm->call_depth,
                 (unsigned)m_buildAbortGen, (unsigned)m_jsEnterGen);
        vm->terminated = true;
        return;
    }
    /* DIAG (temp, pinterest): EWEB_JS_NOBUDGET disables the run-budget and
     * pre-paint watchdog cuts (navigation gen-mismatch above still unwinds) so
     * a slow-but-legitimate React render can run to completion. Used to prove
     * the watchdog cut - not the render itself - is what wedges the page. */
    static int s_nobudget = -1;
    if(s_nobudget < 0) s_nobudget = (getenv("EWEB_JS_NOBUDGET") != nullptr) ? 1 : 0;
    if(s_nobudget)
        return;
    uint64_t now = ticMs();
    if(m_jsRunDeadline != 0 && now > m_jsRunDeadline) {
        if(getenv("MARIO_RUNEXIT") != nullptr)
            fprintf(stderr, "[RUNEXIT] stephook BUDGET terminate pc=%u depth=%d over=%llums\n",
                 (unsigned)vm->pc, (int)vm->call_depth,
                 (unsigned long long)(now - m_jsRunDeadline));
        EWEB_LOG("[ewebview] js: run budget %u ms exceeded - terminating\n",
             (unsigned)(m_jsRunDeadline - m_jsEnterAt));
        /* DIAG (EWEB_SPINDBG): a cut run is either genuinely slow or wedged in a
         * tight loop. Dump the instruction window at the cut point ONCE so the
         * two can be told apart (a spin shows the same handful of pcs across
         * every sample above). Remove with the other temp probes. */
        if(s_spin) {
            static bool s_cut_dumped = false;
            if(!s_cut_dumped) {
                s_cut_dumped = true;
                PC rad = 40;
                if(getenv("MARIO_PCRAD") != nullptr) rad = (PC)atoi(getenv("MARIO_PCRAD"));
                fprintf(stderr, "[spindbg] BUDGET CUT pc=%u depth=%d tag=%s\n",
                        (unsigned)vm->pc, (int)vm->call_depth,
                        vm->dbg_tag != nullptr ? vm->dbg_tag : "-");
                /* Walk the func-scope stack so the JS caller chain of the hot
                 * loop is visible (each frame's function-start pc maps to a
                 * script via the EWEB_SCRIPTDBG pc_range table). */
                fprintf(stderr, "[spindbg] BACKTRACE top=%d:\n", (int)vm->scope_stack_top);
                for(int si = vm->scope_stack_top - 1, shown = 0;
                    si >= 0 && shown < 48; --si) {
                    scope_t* sc = vm->scope_stack[si];
                    if(sc == nullptr || !sc->is_func) continue;
                    PC fpc = (sc->func != nullptr) ? sc->func->pc : 0;
                    const char* nm = (sc->func != nullptr && sc->func->nfe_name != nullptr)
                                     ? sc->func->nfe_name : "-";
                    fprintf(stderr, "[spindbg]  f#%d fpc=%u retpc=%d name=%s\n",
                            shown++, (unsigned)fpc, (int)sc->pc, nm);
                    /* Dump the call site (retpc-2) so the CALL opcode names the
                     * method each frame invoked (split/replace/exec/...). */
                    if(sc->pc > 2) bc_dump_window(&vm->bc, (PC)(sc->pc - 2), 4);
                }
                bc_dump_window(&vm->bc, vm->pc, rad);
                fflush(stderr);
            }
        }
        vm->terminated = true;   /* unwind every nested vm_run frame */
        return;
    }
    /* Pre-paint pages additionally answer to the phase wall clock, so ONE slow
     * script cannot spend the whole run budget before the first paint: the
     * cut is flagged so jsVmExit does not charge it to the watchdog. */
    if(m_jsPrePaintAt != 0 && (now - m_jsPrePaintAt) > kJsPrePaintBudgetMs) {
        if(getenv("MARIO_RUNEXIT") != nullptr)
            fprintf(stderr, "[RUNEXIT] stephook PREPAINT terminate pc=%u depth=%d over=%llums\n",
                 (unsigned)vm->pc, (int)vm->call_depth,
                 (unsigned long long)((now - m_jsPrePaintAt) - kJsPrePaintBudgetMs));
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
    /* SEGV checkpoint: a faulting timer callback skips this tick instead of
     * aborting the browser (see eweb_segv_checkpoint). */
    if(eweb_segv_checkpoint_tag(3) != 0) {
        fprintf(stderr, "[ewebview] js: timer poll faulted (SIGSEGV) - skipping tick\n");
        vm_terminate(m_jsVm);
        /* siglongjmp unwound every vm_run frame without running its restore, so
         * vm->pending_saves dangles at a dead stack frame; clear it (and the
         * abandoned transfer) or the next gc_vars dereferences wild memory. */
        vm_reset_pending(m_jsVm);
        m_jsVm->terminated = false;
        m_jsVm->abort_run = false;
        m_jsVm->propagating_err = nullptr;
        m_jsVm->call_depth = 0;
        m_jsInScript = false;
        m_jsEnterAt = 0;
        return 0;
    }
    jsVmEnter();
    int fired = js_dom_poll_timers(m_jsVm, ticMs());
    eweb_segv_disarm();
    jsVmExit();
    /* TEMP DIAGNOSTIC (MARIO_PROMLEDGER): periodic dump so a stall that begins
     * after the last script (React scheduler suspended on a never-settling
     * promise) is visible during the idle timer phase. */
    if(getenv("MARIO_PROMLEDGER") != nullptr) {
        static uint64_t s_ledger_last = 0;
        uint64_t t = ticMs();
        if(t - s_ledger_last >= 5000) {
            s_ledger_last = t;
            fprintf(stderr, "[promledger] --- timer phase ---\n");
            mario_promise_ledger_dump(m_jsVm);
            fflush(stderr);
        }
    }
    return fired;
}

int EWebEngine::jsFetchPoll()
{
    /* Engine loop step 7b: drain completed async-fetch worker jobs and settle
     * their promises on the VM thread. Same guards as jsPollTimers. */
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled) return 0;
    jsVmEnter();
    int settled = js_web_fetch_poll(m_jsVm);
    jsVmExit();
    return settled;
}

int EWebEngine::jsDrainCeUpgrades()
{
    /* Engine loop step 7c: run the connectedCallback upgrade reaction for
     * custom elements that were INSERTED after their define() already ran its
     * one-shot bulk upgrade (github's React fill commit appends <relative-time>
     * etc. long after define saw matched=0, so without this they never format
     * and stay grey skeletons). Runs JS, so it needs the same VM bracket and
     * SEGV checkpoint as jsPollTimers; early-outs cheaply when nothing was
     * inserted. Returns the number of element names processed so the caller can
     * mark content dirty and repaint the freshly-formatted text. */
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled) return 0;
    if(eweb_segv_checkpoint_tag(4) != 0) {
        vm_terminate(m_jsVm);
        vm_reset_pending(m_jsVm);
        m_jsVm->terminated = false;
        m_jsVm->abort_run = false;
        m_jsVm->propagating_err = nullptr;
        m_jsVm->call_depth = 0;
        m_jsInScript = false;
        m_jsEnterAt = 0;
        /* The fault happened inside js_dom_upgrade_dirty with the dirty list
         * as its input; drop the list or the next tick re-enters with the same
         * corrupted entry and faults forever (engine thread pinned at 100%
         * CPU, timers never polled again - pinterest's shell state). */
        js_dom_clear_dirty_ce(m_jsVm);
        return 0;
    }
    jsVmEnter();
    int n = js_dom_upgrade_dirty(m_jsVm);
    eweb_segv_disarm();
    jsVmExit();
    return n;
}

bool EWebEngine::jsHasImminentTimers()
{
    /* Engine loop step 9 guard. A pure read of the timer table (no callback
     * runs, no GC), so it needs no jsVmEnter bracket. True while a one-shot
     * ~0-ms macrotask - React's MessageChannel scheduler continuation, which
     * js_add_timer stores with remaining_ms 1 - is armed to fire on the next
     * poll. Deferring the repaint while this holds lets the scheduler's task
     * chain run back-to-back (a browser paints it at vsync, not between every
     * slice) and repaint once it drains, instead of paying a full ~4.5 s repaint
     * of a large DOM after each ~17 ms of JS work. */
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled) return false;
    return js_dom_has_imminent_timers(m_jsVm) != 0;
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

/* Serialize a raw id string as a CSS identifier so it can be safely embedded in
 * a `#<id>` selector. document.getElementById must match the id ATTRIBUTE
 * literally, but the litehtml lookup goes through the CSS selector engine, which
 * gives special meaning to ':', '.', '[', etc. An id like React's streaming
 * Suspense markers "B:0"/"S:0" became the selector "#B:0" (id "B" + pseudo-class
 * ":0"), matched nothing, and getElementById returned null - which silently
 * broke $RC()/$RV boundary reveals and every colon-bearing id lookup. Escape per
 * the CSS spec (CSS.escape): backslash-escape punctuation, codepoint-escape a
 * leading digit, pass through [A-Za-z0-9_-] and non-ASCII. */
static std::string css_escape_ident(const std::string& s)
{
    std::string out;
    for(size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if(c >= 0x80) { out.push_back((char)c); continue; }   /* non-ASCII passthrough */
        if(c >= '0' && c <= '9') {
            bool leading = (i == 0) || (i == 1 && s[0] == '-');
            if(leading) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\%x ", (unsigned)c);
                out += buf;
            } else {
                out.push_back((char)c);
            }
            continue;
        }
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-') {
            out.push_back((char)c);
            continue;
        }
        out.push_back('\\');
        out.push_back((char)c);
    }
    return out;
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
    std::string sel = std::string("#") + css_escape_ident(id);
    litehtml::element::ptr el = root->select_one(sel.c_str());
    return (void*)el;   /* nullptr when not found -> JS null */
}

/* Forward decls: the character-data run helpers (defined beside jsElIsPseudo
 * below) are needed here so a text node's nodeValue returns the WHOLE run, not
 * just its first layout chunk. */
static bool jsElIsPseudo(void* el);
static bool jsElIsTextRun(void* el);

/* textContent per the DOM spec is the concatenation of descendant character
 * data ONLY: CSS pseudo-elements (::before/::after) and comments are NOT part
 * of it. litehtml materialises a pseudo's generated `content` as a real child
 * node and its element::get_text() recurses every child, so a primer
 * UnderlineItem span styled with `::before{content:attr(data-content)}` reports
 * "READMEREADME" instead of "README" (the ::before content plus the real text).
 * React's r3 hydration check diffs that against the server string, the mismatch
 * throws #519 and nulls the whole root - the commit columns / sidebar never
 * fill. Collect descendant character-data runs while skipping pseudo-elements
 * and comments so textContent matches a real browser. */
static void js_collect_text_nopseudo(litehtml::element* e, std::string& out)
{
    if(e == nullptr) return;
    if(jsElIsPseudo((void*)e)) return;   /* ::before/::after: not a DOM node */
    if(e->is_comment()) return;          /* comments are excluded from textContent */
    const char* t = e->get_tagName();
    if(t == nullptr || t[0] == 0) {      /* character-data run (el_text/el_space) */
        e->get_text(out);
        return;
    }
    if(e->get_children_count() == 0) {   /* leaf holding its own text (script/style/cdata) */
        e->get_text(out);
        return;
    }
    for(size_t i = 0; i < e->get_children_count(); i++) {
        litehtml::element* c = (litehtml::element*)(void*)e->get_child(i);
        js_collect_text_nopseudo(c, out);
    }
}

char* EWebEngine::jsElGetText(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    std::string text;
    if(jsElIsTextRun(el)) {
        /* The parser splits one HTML character-data run into consecutive
         * el_text/el_space layout nodes (word-wrap + CJK line breaking). A
         * browser exposes the run as a SINGLE text node, and React's hydration
         * diffs nodeValue against the server string: returning only the first
         * chunk ("GitHub" for "GitHub Copilot") desyncs hydration, throws #519
         * and nulls the whole root. Coalesce the run back into one value. */
        e->get_text(text);
        litehtml::element* p = (litehtml::element*)(void*)e->parent();
        if(p != nullptr) {
            bool started = false;
            for(size_t i = 0; i < p->get_children_count(); i++) {
                litehtml::element* c = (litehtml::element*)(void*)p->get_child(i);
                if(c == nullptr) continue;
                if(!started) { if(c == e) started = true; continue; }
                if(jsElIsPseudo((void*)c)) continue;
                if(!jsElIsTextRun((void*)c)) break;   /* run ended */
                c->get_text(text);
            }
        }
    } else if(e->is_comment()) {
        /* A Comment node's OWN textContent/nodeValue/data IS its comment text.
         * React parks Activity/Suspense boundary markers ("&", "/&", "$", ...)
         * in comment nodes and its fe() cursor walk diffs `node.data` against
         * those strings to locate a boundary. js_collect_text_nopseudo() skips
         * comments when gathering an ANCESTOR element's textContent (DOM-spec
         * correct), but when `e` is the comment itself we must return its data,
         * else every marker reads "" -> no boundary matches -> hydration loops
         * (#519 retry storm). el_comment::get_text() appends m_text. */
        e->get_text(text);
    } else {
        js_collect_text_nopseudo(e, text);
        /* DIAG (temp): dump the PHYSICAL child structure of a data-component
         * element whose coalesced textContent looks doubled, to distinguish a
         * real 2nd text node from a text-run-split/coalesce artifact. */
        if(getenv("EWEB_TXTAPPEND_DBG") != nullptr && e->get_attr("data-component", nullptr) != nullptr) {
            int phys = 0; std::string brk;
            for(size_t i = 0; i < e->get_children_count(); i++) {
                litehtml::element* c = (litehtml::element*)(void*)e->get_child(i);
                if(c == nullptr) continue;
                phys++;
                const char* ct = c->get_tagName();
                std::string tt; c->get_text(tt);
                brk += (ct && ct[0] ? std::string("<") + ct + ">" : std::string("TXT"));
                brk += "{" + tt + "} ";
            }
            fprintf(stderr, "[ewebview] TXTPHYS tag=%s dcont=%s physKids=%d text=[%s] brk=%s\n",
                    e->get_tagName() ? e->get_tagName() : "?",
                    e->get_attr("data-content", nullptr) ? e->get_attr("data-content", nullptr) : "-",
                    phys, text.c_str(), brk.c_str());
        }
    }
    return js_strdup_mario(text.c_str());
}

void EWebEngine::jsElSetText(void* ctx, void* el, const char* text)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(el == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    js_set_element_text(e, text != nullptr ? text : "",
                        self != nullptr ? &self->m_jsDetached : nullptr);
    if(self != nullptr) {
        /* Rewriting a <style> element's text swaps its sheet: el_style
         * re-registers with the document, but the restyle itself is driven by
         * the engine loop, so raise the style-update flag here. */
        const char* tg = e->get_tagName();
        if(tg != nullptr && !strcmp(tg, "style"))
            self->jsMarkStyleDirty();
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
static bool jsRestyleSubtree(litehtml::element* e, bool recascade)
{
    if(e == nullptr) return false;
    bool starved = false;
    if(recascade) {
        litehtml::document* doc = e->get_document();
        if(doc != nullptr) starved = doc->style_detached_subtree(e);
    }
    e->parse_styles(true);
    return starved;
}

/* An <img> whose src is set from script may never be appended to the tree
 * (React's preload pattern: new Image(); img.onload=...; img.src=...; the real
 * cover <img> is only appended once that onload fires). A detached img never
 * reaches el_image::parse_styles - the only path that calls load_image - so it
 * never starts loading, its onload never fires, and the follow-up commit never
 * happens: pinterest.com's carousel cover stays a grey placeholder. Mirror a
 * real browser, which fetches img.src whether or not the element is attached:
 * register the element for its load/error event and kick the fetch here. */
static void jsMaybePreloadImage(EWebEngine* self, litehtml::element* e)
{
    if(self == nullptr || e == nullptr) return;
    const char* tn = e->get_tagName();
    if(tn == nullptr || strcmp(tn, "img") != 0) return;
    const char* src = e->get_attr("src", nullptr);
    if(src == nullptr || src[0] == 0) return;
    litehtml::document* doc = e->get_document();
    if(doc == nullptr || doc->container() == nullptr) return;
    std::string full = EWebContainer::getFullURL(&self->m_port, src, self->jsDocumentUrl());
    if(full.empty()) return;
    /* Track the element BEFORE loading so the load/error event can reach it
     * when the fetch settles (see jsFireImgElEvent). An image already in the
     * cache queues no task and produces no result, so its "load" must be fired
     * here or the onload the script parked on would strand (same reasoning as
     * the cached-sheet case in jsMaybeLoadDynamicStylesheet). */
    self->jsNoteImageEl(full, e);
    doc->container()->load_image(full.c_str(), 0, false);
    litehtml::size sz;
    doc->container()->get_image_size(full.c_str(), 0, sz);
    if(sz.width > 0 && sz.height > 0)
        self->jsFireImgElEvent(full, "load");
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
        if(strcmp(name, "class") == 0 || strcmp(name, "id") == 0 ||
           strcmp(name, "style") == 0) {
            /* Rebuild from raw declarations: parse_styles() expands var()/calc()
             * in-place, so reusing the old computed bag preserves stale values.
             * If the budget was exhausted the embedder must schedule a full
             * restyle so nodes beyond the budget still get their vars resolved. */
            if(jsRestyleSubtree(e, true) && self != nullptr) self->jsMarkStyleDirty();
        } else {
            /* [data-x=...] rules key off arbitrary attributes (Primer Stack
             * flips flex-flow via data-direction at hydration time). Hydration
             * churns these attributes, so only queue the budgeted chunked walk
             * (which re-matches selectors) instead of a synchronous subtree
             * restyle that would compete with the hydration pass. */
            litehtml::document* doc = e->get_document();
            if(doc != nullptr && doc->uses_attribute_selector(name) && self != nullptr)
                self->jsMarkStyleDirty();
        }
    }
    if(self != nullptr) {
        const char* idv = e->get_attr("id", nullptr);
        if(idv != nullptr && idv[0] != 0)
            self->recordJsMutation(1, idv, name, value != nullptr ? value : "");
        self->jsMarkLayoutDirty();
    }
    /* A freshly-set img src must start loading even while the node is detached
     * (React's preload-then-append parks the real <img>'s commit on this
     * onload); see jsMaybePreloadImage. */
    if(self != nullptr && strcmp(name, "src") == 0)
        jsMaybePreloadImage(self, e);
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
    void* el = jsScriptStandInEl(m_jsCurScriptUrl);
    if(el != nullptr) {
        m_jsCurScriptEl = el;
        m_jsCurScriptElUrl = m_jsCurScriptUrl;
    }
    return el;
}

/* Stand-in <script src> for a stripped static script tag. Static <script src>
 * tags are removed from the markup before parsing (their bodies run from the
 * ordered queue), so the live tree holds no element for them and the DOM script
 * list (getElementsByTagName("script")/document.scripts) comes back empty. SDKs
 * that locate themselves through that list then dereference undefined and abort
 * (taobao baxia: `ref.parentNode.insertBefore(...)`). Search first so a real or
 * previously materialised node is reused; otherwise create one in <head>. */
void* EWebEngine::jsScriptStandInEl(const std::string& url, const std::string& id)
{
    if(url.empty()) return nullptr;
    litehtml::document* doc = jsActiveDoc();
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    /* An id uniquely identifies the original parser-inserted element. Prefer it
     * over src because valid markup may contain the same bundle URL more than
     * once under different ids. */
    if(!id.empty()) {
        litehtml::element::ptr byId = root->select_one((std::string("#") + css_escape_ident(id)).c_str());
        if(byId != nullptr) return (void*)byId;
    }
    /* Compare scheme-less: markup often carries "//g.alicdn.com/..." while the
     * run queue stores the resolved absolute URL. */
    auto norm = [](const std::string& u) -> std::string {
        if(u.compare(0, 6, "https:") == 0) return u.substr(6);
        if(u.compare(0, 5, "http:") == 0) return u.substr(5);
        return u;
    };
    std::string want = norm(url);
    size_t n = root->get_children_count();
    for(size_t i = 0; i < n; ++i) {
        litehtml::element::ptr c = root->get_child((int)i);
        if(c == nullptr) continue;
        litehtml::elements_vector part = c->select_all(litehtml::tstring("script"));
        for(size_t j = 0; j < part.size(); ++j) {
            if(part[j] == nullptr) continue;
            const char* sa = part[j]->get_attr("src", nullptr);
            if(sa == nullptr || sa[0] == 0 || norm(std::string(sa)) != want) continue;
            const char* ownId = part[j]->get_attr("id", nullptr);
            if(id.empty() || ownId == nullptr || ownId[0] == 0) {
                if(!id.empty()) part[j]->set_attr("id", id.c_str());
                return (void*)part[j];
            }
            if(id == ownId) return (void*)part[j];
        }
    }
    litehtml::string_map attrs;
    litehtml::element::ptr el = doc->create_element("script", attrs);
    if(el == nullptr) return nullptr;
    el->set_attr("src", url.c_str());
    if(!id.empty()) el->set_attr("id", id.c_str());
    litehtml::element::ptr host = root->select_one("head");
    if(host == nullptr) host = root->select_one("body");
    if(host == nullptr) return nullptr;
    host->appendChild(el);
    EWEB_LOG("[ewebview] script stand-in created el=%p id=%s src=%s\n",
             (void*)el, id.empty() ? "(none)" : id.c_str(), url.c_str());
    return (void*)el;
}

void EWebEngine::jsMaterializeScriptStandIns()
{
    for(size_t i = 0; i < m_jsScriptSrcs.size(); ++i) {
        if(m_jsScriptSrcs[i].empty()) continue;
        std::string id = (i < m_jsScriptIds.size())
                             ? m_jsScriptIds[i] : std::string();
        jsScriptStandInEl(m_jsScriptSrcs[i], id);
    }
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
     * the context node itself. Query from `base` and drop `base` from the hit
     * list to get DOM semantics. (Iterating children and calling
     * child->select_all(tstring) instead would miss node types that only
     * override the css_selector& overload - el_script, which backs
     * <script type="application/json"> data islands - because the base
     * element::select_all(tstring) is a no-op.) */
    litehtml::tstring sel(selector);

    /* Fast path for querySelector (max==1, first page): select_one() exits at
     * the first match in document order instead of walking the whole subtree
     * the way select_all() does. The bridge calls this with max==1 for every
     * Element.querySelector, and React's hydration probes do that tens of
     * thousands of times per page - always paying a full subtree scan made it
     * the DOM bridge's second costliest entry after el.matches. Walking the
     * children one by one keeps the base itself out of the result (same as the
     * filter below) and preserves depth-first document order, so the returned
     * element is exactly the first hit select_all would have reported.
     * EWEB_QS_FULLSCAN=1 pins the old always-scan behaviour as an escape hatch
     * (read once; getenv per call would cost more than the path saves). */
    static int s_qs_fullscan = -1;
    if(s_qs_fullscan < 0)
        s_qs_fullscan = (getenv("EWEB_QS_FULLSCAN") != NULL) ? 1 : 0;
    if(!s_qs_fullscan && max == 1 && skip == 0) {
        litehtml::css_selector one(litehtml::media_query_list::ptr(nullptr));
        one.parse(sel);
        size_t nc = base->get_children_count();
        for(size_t i = 0; i < nc; ++i) {
            litehtml::element::ptr c = base->get_child((int)i);
            if(c == nullptr) continue;
            litehtml::element::ptr hit = c->select_one(one);
            if(hit != nullptr) {
                out[0] = (void*)hit;
                return 1;
            }
        }
        return 0;
    }

    litehtml::elements_vector all = base->select_all(sel);
    litehtml::elements_vector hits;
    for(size_t i = 0; i < all.size(); ++i) {
        if(all[i] == nullptr || all[i] == base) continue;
        hits.push_back(all[i]);
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

bool EWebEngine::jsElMatches(void* ctx, void* el, const char* selector)
{
    if(el == nullptr || selector == nullptr || selector[0] == 0) return false;
    /* select() dereferences the element and walks its ancestors (combinator
     * resolution), so a stale wrapper must be rejected before the call - the
     * query-based fallback never dereferenced `el` and got away without this. */
    if(!jsElIsLive(ctx, el)) return false;
    litehtml::element* e = (litehtml::element*)el;
    /* Same parse path html_tag::select_all(tstring) uses. A selector that
     * fails to parse simply matches nothing, mirroring the empty hit list
     * select_all would produce. */
    litehtml::css_selector sel(litehtml::media_query_list::ptr(nullptr));
    sel.parse(litehtml::tstring(selector));
    /* select() returns the select_result flag set; any non-zero flag (direct
     * match, pseudo-class match) is a match, exactly how select_all picks. */
    return e->select(sel) != litehtml::select_no_match;
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

void* EWebEngine::jsCreateComment(void* ctx, const char* text)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;

    /* React parks Suspense boundaries and hydration markers in the tree as
     * comment nodes (document.createComment); without them its client render
     * cannot place boundary markers and bails. el_comment carries m_skip so
     * layout ignores it, exactly like a parsed comment. */
    void* mem = malloc(sizeof(litehtml::el_comment));
    if(mem == nullptr) return nullptr;
    litehtml::el_comment* c = new (mem) litehtml::el_comment(doc);
    if(text != nullptr)
        c->set_data(text);
    return (void*)c;
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

/* A character-data run node (el_text/el_space): not an element, not a comment,
 * not a pseudo. The parser splits one HTML text run into several of these for
 * layout; jsElChildCount/jsElChild/jsElGetText coalesce consecutive ones back
 * into the single text node a browser would expose (React hydration depends on
 * that segmentation matching the server-rendered DOM). */
static bool jsElIsTextRun(void* el)
{
    if(el == nullptr) return false;
    if(jsElIsPseudo(el)) return false;
    litehtml::element* e = (litehtml::element*)el;
    if(e->is_comment()) return false;
    const char* t = e->get_tagName();
    return (t == nullptr || t[0] == 0);
}

int EWebEngine::jsElChildCount(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return 0;
    litehtml::element* e = (litehtml::element*)el;
    int n = 0;
    bool prevText = false;
    for(size_t i = 0; i < e->get_children_count(); i++)
    {
        void* c = (void*)e->get_child(i);
        if(jsElIsPseudo(c)) continue;         /* pseudo: transparent to the DOM */
        bool isText = jsElIsTextRun(c);
        if(!(isText && prevText)) n++;        /* run leaders + every non-text node */
        prevText = isText;
    }
    return n;
}

void* EWebEngine::jsElChild(void* ctx, void* el, int idx)
{
    (void)ctx;
    if(el == nullptr || idx < 0) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    /* EWEB_BODYDBG: pointer-level proof of WHICH body node the JS side walks.
     * rokid's hydration sees body.firstChild=<script> with <main> absent from
     * the sibling chain while the render tree's body is [main, scripts]; log
     * the handle pointer + child count + child0 tag so the JS-view body can be
     * matched against (or distinguished from) the render tree's body. Zero
     * side effects: read-only fprintf. */
    static int s_bodydbg = -1;
    if(s_bodydbg < 0) { const char* bd = getenv("EWEB_BODYDBG"); s_bodydbg = (bd && (bd[0] == '1' || bd[0] == '2')) ? 1 : 0; }
    bool dbg = (s_bodydbg == 1 && idx == 0);
    const char* etag = dbg ? e->get_tagName() : nullptr;
    if(dbg && etag != nullptr && etag[0] != 0 && strcmp(etag, "body") != 0) dbg = false;
    int seen = 0;
    bool prevText = false;
    void* ret = nullptr;
    for(size_t i = 0; i < e->get_children_count(); i++)
    {
        void* c = (void*)e->get_child(i);
        if(jsElIsPseudo(c)) continue;
        bool isText = jsElIsTextRun(c);
        bool leader = !(isText && prevText);
        prevText = isText;
        if(!leader) continue;                 /* text-run continuation: hidden */
        if(seen == idx) { ret = c; break; }
        seen++;
    }
    if(dbg) {
        const char* ctag = nullptr;
        if(ret != nullptr) ctag = ((litehtml::element*)ret)->get_tagName();
        fprintf(stderr, "[bodydbg] jsElChild body=%p idx0 count=%d child0=%s\n",
                (void*)e, (int)e->get_children_count(),
                (ret == nullptr) ? "NULL" : ((ctag == nullptr || ctag[0] == 0) ? "#text" : ctag));
        /* EWEB_BODYDBG=2: also dump the whole body child chain (tag or #text)
         * so the JS-view sequence can be diffed against the raw SSR HTML. */
        static int s_full = -1;
        if(s_full < 0) { const char* bf = getenv("EWEB_BODYDBG"); s_full = (bf && bf[0] == '2') ? 1 : 0; }
        if(s_full == 1) {
            fprintf(stderr, "[bodydbg]   chain:");
            for(size_t k = 0; k < e->get_children_count(); k++) {
                litehtml::element* cc = (litehtml::element*)e->get_child((int)k);
                const char* tt = cc ? cc->get_tagName() : nullptr;
                fprintf(stderr, " %s", (cc == nullptr) ? "NULL"
                        : ((tt == nullptr || tt[0] == 0) ? "#text" : tt));
            }
            fprintf(stderr, "\n");
        }
    }
    return ret;
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

/* EWEB_DOMSIG: print the childNodes signature (nodeType + tag/id, text-run
 * coalesced exactly like jsElChildCount) two levels deep, so the pre-hydration
 * DOM can be diffed against a real browser's capture of the same SSR HTML. */
static void ewebDumpDomSig(litehtml::element* e, int depth)
{
    if(e == nullptr || depth > 4) return;
    bool prevText = false;
    for(size_t i = 0; i < e->get_children_count(); i++) {
        litehtml::element* c = (litehtml::element*)e->get_child((int)i);
        if(c == nullptr || jsElIsPseudo((void*)c)) continue;
        bool isText = jsElIsTextRun((void*)c);
        bool leader = !(isText && prevText);
        prevText = isText;
        if(!leader) continue;
        const char* indent = (depth == 0) ? "" : (depth == 1) ? "  " : (depth == 2) ? "    " : (depth == 3) ? "      " : "        ";
        if(c->is_comment()) {
            litehtml::tstring txt;
            c->get_text(txt);
            fprintf(stderr, "[domsig] %s#comment %.40s\n", indent, txt.c_str());
        } else if(isText) {
            fprintf(stderr, "[domsig] %s#text\n", indent);
        } else {
            litehtml::position pb = c->get_placement();
            litehtml::css_length ch = c->get_css_height();
            const char* hspec = ch.is_predefined() ? "auto"
                              : (ch.units() == litehtml::css_units_percentage) ? "pct" : "px";
            fprintf(stderr, "[domsig] %s%s ptr=%p id=%s cls=%.24s disp=%s pos=%s box=%d,%d %dx%d h=%s(%g)\n",
                    indent, c->get_tagName(), (void*)c,
                    c->get_attr("id", ""), c->get_attr("class", ""),
                    c->get_style_property(_t("display"), false, _t("?")),
                    c->get_style_property(_t("position"), false, _t("?")),
                    pb.x, pb.y, pb.width, pb.height,
                    hspec, (double)(ch.is_predefined() ? 0 : ch.val()));
        }
        if(!isText && !c->is_comment()) ewebDumpDomSig(c, depth + 1);
    }
}

/* EWEB_HEADDUMP: print EVERY raw child of <head> (no pseudo skipping, no
 * text-run coalescing - React's hydration pointer walks the bridge's filtered
 * view but divergences can also come from nodes the bridge hides) with full
 * attribute tables, so the head can be diffed node-by-node against raw SSR. */
static void ewebDumpHeadAttrs(litehtml::element* head)
{
    if(head == nullptr) return;
    for(size_t i = 0; i < head->get_children_count(); i++) {
        litehtml::element* c = (litehtml::element*)head->get_child((int)i);
        if(c == nullptr) { fprintf(stderr, "[headdump] %zu NULL\n", i); continue; }
        const char* tag = c->get_tagName();
        bool pseudo = jsElIsPseudo((void*)c);
        bool text = jsElIsTextRun((void*)c);
        if(c->is_comment()) {
            litehtml::tstring txt;
            c->get_text(txt);
            fprintf(stderr, "[headdump] %zu #comment %.40s\n", i, txt.c_str());
        } else if(text) {
            litehtml::tstring txt;
            c->get_text(txt);
            fprintf(stderr, "[headdump] %zu #text %.30s\n", i, txt.c_str());
        } else {
            fprintf(stderr, "[headdump] %zu %s%s ptr=%p", i,
                    pseudo ? "(pseudo)" : "", (tag == nullptr || tag[0] == 0) ? "?" : tag,
                    (void*)c);
            const litehtml::string_map* attrs = c->eweb_attrs();
            if(attrs != nullptr) {
                for(const auto& kv : *attrs)
                    fprintf(stderr, " %s=%.60s", kv.first.c_str(), kv.second.c_str());
            }
            fprintf(stderr, "\n");
        }
    }
}

bool EWebEngine::jsElIsComment(void* ctx, void* el)
{
    (void)ctx;
    if(el == nullptr) return false;
    return ((litehtml::element*)el)->is_comment();
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

/* A <link rel="stylesheet"> spliced in by script (createElement('link');
 * l.href=...; head.appendChild(l)) never goes through the parser's el_link ->
 * import_css hook, so its sheet would never load and any component styled
 * only by that CSS renders unstyled - rokid.com injects the stylesheet that
 * lays out its nav/footer exactly this way. Mirror the static path here:
 * record the media attribute and queue the fetch; loadCSSContent() then
 * applies the sheet to the live document and re-styles it. */
static void jsMaybeLoadDynamicStylesheet(EWebEngine* self, litehtml::element* c)
{
    if(self == nullptr || c == nullptr) return;
    const char* tn = c->get_tagName();
    if(tn == nullptr || strcmp(tn, "link") != 0) return;
    const char* rel = c->get_attr("rel", nullptr);
    if(rel == nullptr || strcasecmp(rel, "stylesheet") != 0) return;
    const char* href = c->get_attr("href", nullptr);
    if(href == nullptr || href[0] == 0) return;
    std::string full = EWebContainer::getFullURL(&self->m_port, href, self->jsDocumentUrl());
    if(full.empty()) return;
    const char* media = c->get_attr("media", nullptr);
    /* Track the element BEFORE loadCSS so the load/error event can reach it when
     * the fetch settles (see jsFireLinkElEvent). A sheet that is already in the
     * cache never queues a task and therefore never produces a result, so its
     * "load" must be fired here or the chunk promise that parks on link.onload
     * (Rspack miniCss) would strand. */
    bool cached = self->hasSeenCSS(full);
    if(getenv("EWEB_MODPATCH_DBG") != nullptr)
        fprintf(stderr, "[csslink] MAYBE full=%s cached=%d\n", full.c_str(), cached ? 1 : 0);
    self->setCSSMedia(full, media ? std::string(media) : std::string());
    self->jsNoteDynamicStylesheet(full, c);
    self->loadCSS(full);
    if(cached)
        self->jsFireLinkElEvent(full, "load");
}

/* Env-gated (EWEB_MUTDBG) tree-mutation trace: logs parent/child/ref tags for
 * every appendChild/insertBefore so a wrong final DOM order can be attributed
 * to the exact call that produced it. */
static void mutdbg_log(const char* op, litehtml::element* p, litehtml::element* c, litehtml::element* r) {
    static int on = -1;
    if(on < 0) on = (getenv("EWEB_MUTDBG") != nullptr);
    if(!on) return;
    auto tg = [](litehtml::element* e) -> const char* {
        if(e == nullptr) return "null";
        const char* t = e->get_tagName();
        return (t != nullptr && t[0] != 0) ? t : "#text";
    };
    auto idof = [](litehtml::element* e) -> const char* {
        if(e == nullptr) return "";
        const char* v = e->get_attr("id", nullptr);
        return (v != nullptr) ? v : "";
    };
    fprintf(stderr, "[MUT] %s parent=<%s%s> child=<%s%s> ref=<%s%s>\n", op,
            tg(p), idof(p), tg(c), idof(c), tg(r), idof(r));
}

bool EWebEngine::jsElAppendChild(void* ctx, void* parent, void* child)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(parent == nullptr || child == nullptr) return false;
    mutdbg_log("append", (litehtml::element*)parent, (litehtml::element*)child, nullptr);
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
    litehtml::element* p = (litehtml::element*)parent;
    litehtml::element* c = (litehtml::element*)child;
    if(!p->appendChild(c)) return false;
    /* DIAG (temp): catch the README->READMEREADME text duplication that trips
     * React's r3 textContent hydration check (#519). Fires only when a text run
     * is appended to a parent that ALREADY has a text-run child, i.e. the exact
     * duplicate-append. Gated by EWEB_TXTAPPEND_DBG so release stays quiet. */
    if(getenv("EWEB_TXTAPPEND_DBG") != nullptr) {
        const char* ctn = c->get_tagName();
        if(ctn == nullptr || ctn[0] == 0) {           /* child is el_text */
            int txtSibs = 0; std::string sibText;
            for(size_t i = 0; i < p->get_children_count(); i++) {
                litehtml::element* s = (litehtml::element*)(void*)p->get_child(i);
                if(s == nullptr || s == c) continue;
                const char* st = s->get_tagName();
                if(st == nullptr || st[0] == 0) { txtSibs++; std::string t2; s->get_text(t2); sibText += t2; }
            }
            if(txtSibs > 0) {
                std::string ctext; c->get_text(ctext);
                const char* ptn = p->get_tagName();
                const char* pdc = p->get_attr("data-component", nullptr);
                const char* pdcont = p->get_attr("data-content", nullptr);
                fprintf(stderr, "[ewebview] TXTDUP parent=%s dc=%s dcont=%s newtext=[%s] prevTxtSibs=%d prevText=[%s]\n",
                        ptn ? ptn : "?", pdc ? pdc : "-", pdcont ? pdcont : "-",
                        ctext.c_str(), txtSibs, sibText.c_str());
            }
        }
    }
    /* A node built by createElement never went through the document-creation
     * stylesheet walks, so match master/attribute/document styles against the
     * inserted subtree first - without it a scripted <div> keeps html_tag's
     * default inline display and stacks sideways. Already styled nodes (a
     * moved subtree) are skipped inside. */
    bool attach_starved = false;
    if(litehtml::document* d = c->get_document()) {
        attach_starved = d->style_detached_subtree(c);
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
        /* A runtime-injected <style> contributes its sheet inside
         * style_detached_subtree's parse_attributes -> document::add_stylesheet
         * (which parses straight into m_styles once the creation sweep is done).
         * The sheet's selectors target elements that already exist elsewhere in
         * the tree, so styling the new <style> node is not enough: raise the
         * style-update flag to drive a full re-cascade that re-matches them
         * (gestalt's per-field `#label-x > div { color: transparent }`). */
        if(tn != nullptr && strcmp(tn, "style") == 0)
            self->jsMarkStyleDirty();
        /* The attach walk is budgeted: a detached-built subtree larger than the
         * budget keeps starved styles (var() declarations its detached parse
         * dropped) on the nodes past the cut. A full chunked restyle is the
         * only pass that re-resolves them against the live ancestor chain. */
        if(attach_starved)
            self->jsMarkStyleDirty();
        jsMaybeLoadDynamicStylesheet(self, c);
        self->jsMarkLayoutDirty();
    }
    return true;
}

bool EWebEngine::jsElInsertBefore(void* ctx, void* parent, void* child, void* ref)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(parent == nullptr || child == nullptr) return false;
    mutdbg_log("insertBefore", (litehtml::element*)parent, (litehtml::element*)child, (litehtml::element*)ref);
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
    litehtml::element* p = (litehtml::element*)parent;
    litehtml::element* c = (litehtml::element*)child;
    /* html_tag::insertBefore appends when ref is null or is not a child of
     * parent, which is exactly the DOM contract. */
    if(!p->insertBefore(c, (litehtml::element*)ref))
        return false;
    /* DIAG (temp): mirror of the jsElAppendChild TXTDUP probe - React splices
     * hydration text via insertBefore(node,null). */
    if(getenv("EWEB_TXTAPPEND_DBG") != nullptr) {
        const char* ctn = c->get_tagName();
        if(ctn == nullptr || ctn[0] == 0) {
            int txtSibs = 0; std::string sibText;
            for(size_t i = 0; i < p->get_children_count(); i++) {
                litehtml::element* s = (litehtml::element*)(void*)p->get_child(i);
                if(s == nullptr || s == c) continue;
                const char* st = s->get_tagName();
                if(st == nullptr || st[0] == 0) { txtSibs++; std::string t2; s->get_text(t2); sibText += t2; }
            }
            if(txtSibs > 0) {
                std::string ctext; c->get_text(ctext);
                const char* ptn = p->get_tagName();
                const char* pdcont = p->get_attr("data-content", nullptr);
                fprintf(stderr, "[ewebview] TXTDUP-IB parent=%s dcont=%s newtext=[%s] prevTxtSibs=%d prevText=[%s]\n",
                        ptn ? ptn : "?", pdcont ? pdcont : "-", ctext.c_str(), txtSibs, sibText.c_str());
            }
        }
    }
    bool attach_starved = false;
    if(litehtml::document* d = c->get_document()) {
        attach_starved = d->style_detached_subtree(c);
    }
    c->parse_styles(false);
    if(self != nullptr) {
        js_unpark(self->m_jsDetached, c);
        /* See jsElAppendChild: a <script> spliced in via insertBefore fetches
         * and runs the same way. */
        const char* tn = c->get_tagName();
        if(tn != nullptr && strcmp(tn, "script") == 0)
            self->jsDynamicScriptInserted(c);
        if(attach_starved)
            self->jsMarkStyleDirty();
        jsMaybeLoadDynamicStylesheet(self, c);
        self->jsMarkLayoutDirty();
    }
    return true;
}

bool EWebEngine::jsElRemoveChild(void* ctx, void* parent, void* child)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(parent == nullptr || child == nullptr) return false;
    mutdbg_log("remove", (litehtml::element*)parent, (litehtml::element*)child, nullptr);
    /* See jsElAppendChild: never deref a dangling handle. */
    if(!jsElIsLive(ctx, parent) || !jsElIsLive(ctx, child)) {
        EWEB_LOG("[ewebview] removeChild rejected stale handle: parent=%p child=%p\n", parent, child);
        return false;
    }
    litehtml::element* p = (litehtml::element*)parent;
    litehtml::element* c = (litehtml::element*)child;
    if(!p->removeChild(c)) return false;
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
        if(strcmp(name, "class") == 0 || strcmp(name, "id") == 0 ||
           strcmp(name, "style") == 0) {
            /* Rebuild from raw declarations: parse_styles() expands var()/calc()
             * in-place, so reusing the old computed bag preserves stale values. */
            if(jsRestyleSubtree(e, true) && self != nullptr) self->jsMarkStyleDirty();
        } else {
            litehtml::document* doc = e->get_document();
            if(doc != nullptr && doc->uses_attribute_selector(name) && self != nullptr)
                self->jsMarkStyleDirty();
        }
    }
    if(self != nullptr) self->jsMarkLayoutDirty();
}

void EWebEngine::jsElGetRect(void* ctx, void* el, int* x, int* y, int* w, int* h)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(x) *x = 0;
    if(y) *y = 0;
    if(w) *w = 0;
    if(h) *h = 0;
    if(el == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    if(e->get_display() == litehtml::display_none) return;
    /* get_placement() accumulates the per-element m_pos, and m_pos is the
     * CONTENT box (html_tag.h: "m_pos.height is always the content box"; and
     * render() does m_pos.x = origin + content_margins_left(), so the
     * accumulated x/y is the content-box corner). The bridge contract
     * (js_dom.h el_get_rect) and the CSSOM want the BORDER box: shift the
     * origin back over padding+border ONLY, and grow the size by padding+
     * border. Subtracting the margin as well (content_margins_left() folds it
     * in) landed the origin on the MARGIN box, so getBoundingClientRect()/
     * offsetLeft of any element with a margin under-reported its origin by
     * that margin while the size stayed border-box -- an inconsistent box that
     * made measuring scripts see a child's border box overflow its parent's
     * (Pinterest's signup card, margin:40, read left=0 while the button inside
     * read left=40). Reporting the raw content box instead collapsed a padded
     * field (gestalt's 24px-padded birthday wrapper). */
    litehtml::position p = e->get_placement();
    litehtml::margins pad = e->get_paddings();
    litehtml::margins brd = e->get_borders();
    /* Only a REPLACED widget-mode control folds its chrome into m_pos, so its
     * m_pos is already the BORDER box (unlike html_tag, whose m_pos is the
     * content box). Applying the content->border correction below would
     * double-count the padding+border and inflate getBoundingClientRect() by
     * one chrome (a 55px field read as 87px). Report those as-is.
     * A container-mode <button> (display:flex or real element children) is NOT
     * replaced: it lays out through html_tag::render, so its m_pos is the
     * CONTENT box and must take the correction below. Treating it as a border
     * box under-reported the rect by one padding+border once the UA sheet sized
     * buttons border-box (Pinterest's "Continue with Google" read 340x30 for a
     * 402x48 box). */
    void* fw = e->eweb_form_widget();
    if(fw != nullptr && ((eweb_el_input*)fw)->is_replaced()) {
        if(x) *x = p.x;
        if(y) *y = p.y;
        if(w) *w = p.width;
        if(h) *h = p.height;
        return;
    }
    if(x) *x = p.x - pad.left - brd.left;
    if(y) *y = p.y - pad.top - brd.top;
    if(w) *w = p.width + pad.left + pad.right + brd.left + brd.right;
    if(h) *h = p.height + pad.top + pad.bottom + brd.top + brd.bottom;
    /* position:sticky pins shift the element's USED position: Chrome reports
     * the pinned box through both offsetTop and getBoundingClientRect, and
     * the paint path draws it shifted too, so the rect bridge must match.
     * The layout box above stays unshifted; add the scroll-driven pin here.
     * Scroll/viewport come from the same source jsGetScroll/jsWebGetViewport
     * use, so fixed-style viewport semantics stay consistent. */
    if(e->get_element_position() == litehtml::element_position_sticky &&
       self != nullptr) {
        int sdy = 0;
        e->eweb_sticky_offset(self->m_engineScrollY, self->m_clientHeight, &sdy);
        if(sdy != 0 && y) *y += sdy;
    }
}

/* getBoundingClientRect / IntersectionObserver report the border box UNDER the
 * element's CSS transform (own plus ancestors), while the offset* family keeps
 * the untransformed layout box jsElGetRect reports. The transform is therefore
 * applied only on this viewport-rect path (js_dom.c el_viewport_rect). */
void EWebEngine::jsElTransformRect(void* ctx, void* el, int* x, int* y, int* w, int* h)
{
    (void)ctx;
    if(el == nullptr || x == nullptr || y == nullptr || w == nullptr || h == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    litehtml::position bb;
    bb.x = *x;
    bb.y = *y;
    bb.width = *w;
    bb.height = *h;
    if(e->eweb_transform_bounds(bb)) {
        *x = bb.x;
        *y = bb.y;
        *w = bb.width;
        *h = bb.height;
    }
}

/* offsetLeft/offsetTop are relative to the offsetParent's border box (the
 * nearest positioned ancestor, falling back to body); jsElGetRect reports
 * document coordinates, so geom_get subtracts this origin. Fixed elements
 * have a null offsetParent and keep document coordinates. */
void EWebEngine::jsElOffsetParentOrigin(void* ctx, void* el, int* x, int* y)
{
    (void)ctx;
    if(x) *x = 0;
    if(y) *y = 0;
    if(el == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    if(e->get_element_position() == litehtml::element_position_fixed) return;
    for(litehtml::element* p = e->parent(); p != nullptr; p = p->parent()) {
        if(p->get_element_position() != litehtml::element_position_static || p->is_body()) {
            litehtml::position pl = p->get_placement();
            void* fw = p->eweb_form_widget();
            if(fw != nullptr && ((eweb_el_input*)fw)->is_replaced()) {
                /* A replaced widget-mode control's m_pos is the border box. */
                if(x) *x = pl.x;
                if(y) *y = pl.y;
            } else {
                litehtml::margins pad = p->get_paddings();
                litehtml::margins brd = p->get_borders();
                if(x) *x = pl.x - pad.left - brd.left;
                if(y) *y = pl.y - pad.top - brd.top;
            }
            return;
        }
    }
}

/* document.elementFromPoint: the topmost element at a VIEWPORT point (client
 * coordinates). The hit walker runs in document coordinates and needs the live
 * scroll to pin position:sticky boxes, so hand the engine's scroll to the
 * document first (same as the mouse dispatch path). Points outside the
 * viewport report null, like a real browser. */
void* EWebEngine::jsElFromPoint(void* ctx, int x, int y)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    if(x < 0 || y < 0 || x >= self->m_clientWidth || y >= self->m_clientHeight) return nullptr;
    litehtml::document* doc = self->jsActiveDoc();
    if(doc == nullptr) return nullptr;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return nullptr;
    doc->set_hit_scroll(self->m_engineScrollX, self->m_engineScrollY);
    litehtml::element::ptr t = root->get_element_by_point(
        x + self->m_engineScrollX, y + self->m_engineScrollY, x, y);
    return (void*)(litehtml::element*)t;
}

/* clientWidth/clientHeight: the padding box (content + padding), i.e. the
 * border box jsElGetRect reports minus the borders. */
void EWebEngine::jsElGetClientSize(void* ctx, void* el, int* w, int* h)
{
    (void)ctx;
    if(w) *w = 0;
    if(h) *h = 0;
    if(el == nullptr) return;
    litehtml::element* e = (litehtml::element*)el;
    if(e->get_display() == litehtml::display_none) return;
    litehtml::position p = e->get_placement();
    litehtml::margins pad = e->get_paddings();
    void* fw = e->eweb_form_widget();
    /* Only a replaced widget-mode control has m_pos == border box; a
     * container-mode <button> lays out through html_tag (m_pos == content box)
     * and takes the padding-box sum below, same as any html_tag. */
    if(fw != nullptr && ((eweb_el_input*)fw)->is_replaced()) {
        /* m_pos is the border box: the padding box is it minus the borders. */
        litehtml::margins brd = e->get_borders();
        if(w) *w = p.width - brd.left - brd.right;
        if(h) *h = p.height - brd.top - brd.bottom;
        return;
    }
    if(w) *w = p.width + pad.left + pad.right;
    if(h) *h = p.height + pad.top + pad.bottom;
}

/* Live page scroll offset, so the DOM bridge can report viewport-relative
 * bounding rects and a viewport-sized scrolling element. */
void EWebEngine::jsGetScroll(void* ctx, int* x, int* y)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(x) *x = (self != nullptr) ? self->m_engineScrollX : 0;
    if(y) *y = (self != nullptr) ? self->m_engineScrollY : 0;
}

/* scrollWidth/scrollHeight of the scrolling element: the document's content
 * extent (never less than the viewport, per CSSOM), not the viewport box
 * el_get_rect reports for html/body. jsActiveDoc() so timer callbacks after
 * the swap read the visible document's geometry. */
/* Temporary diagnostic (EWEB_DOCID): list every non-visible-overflow box so
 * the ancestor that stops html_tag::calc_document_size from recursing into
 * the tall marketing content is visible. Remove once the root cause is fixed. */
static void ewebDumpOverflowChain(litehtml::element* el, int depth)
{
    if(el == nullptr || depth > 48) return;
    if(el->get_overflow() != litehtml::overflow_visible) {
        const litehtml::position& pos = el->get_position();
        const char* cls = el->get_attr("class");
        fprintf(stderr, "[OVDUMP] d=%d ov=%d tag=%s cls=%s box=%d,%d %dx%d bottom=%d\n",
                depth, (int)el->get_overflow(), el->get_tagName(),
                (cls != nullptr) ? cls : "", pos.x, pos.y, pos.width, pos.height,
                pos.bottom());
    }
    const char* tn = el->get_tagName();
    if(tn == nullptr || tn[0] == 0 || strcmp(tn, "::") == 0) return;
    litehtml::html_tag* tag = static_cast<litehtml::html_tag*>(el);
    for(auto& ch : tag->children())
        ewebDumpOverflowChain((litehtml::element*)ch, depth + 1);
}

void EWebEngine::jsGetDocumentSize(void* ctx, int* w, int* h)
{
    EWebEngine* self = (EWebEngine*)ctx;
    litehtml::document* doc = (self != nullptr) ? self->jsActiveDoc() : nullptr;
    if(getenv("EWEB_DOCID") != nullptr) {
        fprintf(stderr, "[DOCID] doc=%p h=%d build=%p h=%d active=%p h=%d\n",
                (void*)(self != nullptr ? self->m_doc : nullptr),
                (self != nullptr && self->m_doc != nullptr) ? self->m_doc->height() : -1,
                (void*)(self != nullptr ? self->m_buildDoc : nullptr),
                (self != nullptr && self->m_buildDoc != nullptr) ? self->m_buildDoc->height() : -1,
                (void*)doc, (doc != nullptr) ? doc->height() : -1);
        if(doc != nullptr && doc->root() != nullptr) {
            const litehtml::position& rp = doc->root()->get_position();
            fprintf(stderr, "[DOCID] rootbox=%d,%d %dx%d bottom=%d\n",
                    rp.x, rp.y, rp.width, rp.height, rp.bottom());
            ewebDumpOverflowChain((litehtml::element*)doc->root(), 0);
        }
    }
    int dw = (doc != nullptr) ? doc->width() : 0;
    int dh = (doc != nullptr) ? doc->height() : 0;
    int vw = 0, vh = 0;
    jsWebGetViewport(ctx, &vw, &vh);
    if(dw < vw) dw = vw;
    if(dh < vh) dh = vh;
    if(w) *w = dw;
    if(h) *h = dh;
}

char* EWebEngine::jsElGetStyle(void* ctx, void* el, const char* prop)
{
    (void)ctx;
    if(el == nullptr || prop == nullptr || prop[0] == 0) return nullptr;
    /* get_style_property resolves the used value; per CSS only INHERITED
     * properties may fall back to an ancestor's computed value -- for the
     * rest an absent own declaration means the initial value, so passing
     * inherited=true unconditionally leaked e.g. an ancestor's raw var()
     * padding into a child's getComputedStyle(). Non-const on html_tag
     * because it fills a per-element property cache. */
    const char* v = ((litehtml::element*)el)->get_style_property(
        prop, litehtml::css_property_inherited(prop), nullptr);
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

int EWebEngine::jsWebGetColorScheme(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || self->m_container == nullptr)
        return 1;   /* unknown: light, the same default the CSS side uses */
    /* The container is the single source of truth: stylesheet media queries
     * and matchMedia() must report one scheme or the page's JS theme switch
     * fights its own CSS (github.com gates dark mode on both). */
    litehtml::media_features feat;
    self->m_container->get_media_features(feat);
    return feat.color_scheme;
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

void EWebEngine::jsWebUpdateUrl(void* ctx, const char* url)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || url == nullptr || url[0] == 0) return;
    /* history.pushState/replaceState: adopt the new URL WITHOUT refetching.
     * Resolve a bare path against the current document, then publish it so
     * location.* and the address bar track the SPA route. A full navigation
     * here would tear the running VM down and reload the page - exactly the
     * loop app-router's post-hydration replaceState("/zh-CN") would cause. */
    std::string full = EWebContainer::normalizeURL(&self->m_port, std::string(url),
                                                   self->jsDocumentUrl());
    if(full.empty()) full = url;
    self->m_currentHtmlUrl = full;
    if(!self->m_buildHtmlUrl.empty()) self->m_buildHtmlUrl = full;
    { EWebUiEvent uev; uev.kind = EUET_URL; uev.text = full; self->postUiEvent(uev); }
    EWEB_LOG("[ewebview] js: history url -> %s\n", full.c_str());
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

    /* Relative refs resolve against the document the script is running in,
     * exactly like <script src> does. */
    std::string full = EWebContainer::getFullURL(&self->m_port, url, self->jsDocumentUrl());
    if(full.empty()) return false;

    if(!jsWebRequestRaw(&self->m_port, method, full, headers, body,
                        self->jsDocumentUrl(), status, out_body, out_headers))
        return false;
    /* The bridge adopts both outputs and frees them with mario_free, so they
     * must be mario-owned; the raw helper malloc's (worker-thread safe). */
    if(out_body != nullptr && *out_body != nullptr) {
        char* mb = (char*)mario_malloc((uint32_t)strlen(*out_body) + 1);
        if(mb == nullptr) { free(*out_body); *out_body = nullptr; return false; }
        strcpy(mb, *out_body);
        free(*out_body);
        *out_body = mb;
    }
    if(out_headers != nullptr && *out_headers != nullptr) {
        char* mh = js_strdup_mario(*out_headers);
        free(*out_headers);
        *out_headers = mh;
    }
    return true;
}

bool EWebEngine::jsWebRequestRaw(const eweb_port_t* port, const char* method,
                                 const std::string& url, const char* headers,
                                 const char* body, const std::string& base_url,
                                 int* status, char** out_body, char** out_headers)
{
    if(port == nullptr || url.empty()) return false;
    if(status != nullptr) *status = 0;
    if(out_body != nullptr) *out_body = nullptr;
    if(out_headers != nullptr) *out_headers = nullptr;
    if(!port->net.request || !port->net.free_response) return false;

    std::string full = url;

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
                cur, base_url, false);
        std::vector<eweb_http_header_t> send = hdrs;
        std::string cookie_storage;   /* keeps cookie.c_str() alive per hop */
        if(!cookie.empty()) {
            cookie_storage = cookie;
            eweb_http_header_t ch;
            ch.key = "Cookie";
            ch.value = cookie_storage.c_str();
            send.push_back(ch);
        }
        /* Env-gated: show the Cookie the jar produced for this hop so a
         * server-side CSRF/session refusal can be attributed to a missing
         * cookie versus a missing header. */
        if(getenv("EWEB_NETDBG") != NULL)
            fprintf(stderr, "[netdbg] xhr %s %s cookie=%s body=%d\n",
                    cur_method.c_str(), cur.c_str(),
                    cookie.empty() ? "(none)" : cookie.c_str(), cur_body_len);

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

    /* Plain malloc'd NUL-terminated copies: safe to build on a worker thread
     * (mario's allocator is engine-thread only); jsWebRequest re-owns them. */
    if(out_body != nullptr) {
        int n = resp.body_size > 0 ? resp.body_size : 0;
        char* ob = (char*)malloc((size_t)n + 1);
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
        *out_headers = strdup(block.c_str());
    }
    port->net.free_response(port->net.ud, &resp);
    return true;
}

/* ==================================================================
 * ES module loader (mario _resolve_m_func / _load_m_func)
 * ================================================================== */

static EWebEngine* js_engine_of_vm(vm_t* vm)
{
    return (vm != nullptr) ? (EWebEngine*)vm->on_step_data : nullptr;
}

/* ------------------------------------------------------------------
 * Background ES-module prefetch.
 *
 * The VM evaluates `import` graphs synchronously: jsModuleLoad blocks on one
 * HTTP round trip per module (~350ms each), so github's 83-module graph cost
 * ~30s of wall time before react could even start rendering. Browsers instead
 * scan sources for specifiers and fetch the graph ahead of execution.
 *
 * This mirrors that: a small pthread pool pulls module bodies into a
 * URL-keyed cache; every fetched body is regex-scanned for further import
 * specifiers (static, dynamic and re-exports) which are resolved and queued
 * in turn, so the graph fans out without waiting for evaluation. jsModuleLoad
 * consumes the cache: hit = no blocking, in-flight = wait on the worker
 * instead of issuing a duplicate request, miss = the original synchronous
 * fetch. Workers only use the port net callbacks (per-request state) and the
 * mutex-guarded cookie jar, so they never touch engine-thread-only state.
 * ------------------------------------------------------------------ */
struct EWebEngine::ModulePrefetch {
    struct Entry {
        int   state;      /* 0 = queued/in-flight, 1 = fetched, -1 = failed */
        int   status;     /* HTTP status once fetched */
        char* body;       /* malloc'd NUL-terminated body, nullptr until done */
    };
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    std::map<std::string, Entry> cache;
    std::deque<std::string> queue;
    std::vector<pthread_t> workers;
    bool stop;
    int  hits, waits, misses, fetched, failed, discovered;

    ModulePrefetch() : stop(false), hits(0), waits(0), misses(0),
                       fetched(0), failed(0), discovered(0) {
        pthread_mutex_init(&lock, nullptr);
        pthread_cond_init(&cv, nullptr);
    }
};

static bool js_prefetch_disabled()
{
    const char* e = getenv("EWEB_MODULE_PREFETCH");
    return (e != nullptr && e[0] == '0');
}

void* EWebEngine::jsPrefetchWorker(void* arg)
{
    EWebEngine* self = (EWebEngine*)arg;
    ModulePrefetch* pf = self->m_jsPrefetch;
    for(;;) {
        std::string url;
        pthread_mutex_lock(&pf->lock);
        for(;;) {
            if(pf->stop) { pthread_mutex_unlock(&pf->lock); return nullptr; }
            if(!pf->queue.empty()) { url = pf->queue.front(); pf->queue.pop_front(); break; }
            pthread_cond_wait(&pf->cv, &pf->lock);
        }
        pthread_mutex_unlock(&pf->lock);

        int status = 0;
        char* body = nullptr;
        bool ok = jsWebRequestRaw(&self->m_port, "GET", url, "Accept: */*",
                                  nullptr, self->jsDocumentUrl(), &status, &body, nullptr);
        ok = ok && body != nullptr && status >= 200 && status < 300;

        pthread_mutex_lock(&pf->lock);
        ModulePrefetch::Entry& e = pf->cache[url];
        if(ok) {
            e.state = 1;
            e.status = status;
            e.body = body;   /* cache adopts the malloc'd body */
            pf->fetched++;
        } else {
            if(body != nullptr) free(body);
            e.state = -1;
            e.status = status;
            e.body = nullptr;
            pf->failed++;
        }
        pthread_cond_broadcast(&pf->cv);
        pthread_mutex_unlock(&pf->lock);

        if(ok) self->jsPrefetchScan(url, body);
    }
    return nullptr;
}

/* Extract import/export specifiers from a module body and queue them.
 * Patterns seen in the wild: `import x from "spec"`, `import "spec"`,
 * `import(...)`, `export * from "spec"`, `export {a} from "spec"`. A hand
 * rolled linear scanner (not std::regex): these bundles are 500KB+ minified
 * single lines where an alternation regex risks pathological backtracking.
 * Over-matching is harmless: anything that fails to fetch is dropped, and
 * jsModuleLoad never reads a cache entry the VM did not ask for. */
void EWebEngine::jsPrefetchScan(const std::string& module_url, const char* body)
{
    if(body == nullptr) return;
    const char* p = body;
    int count = 0;
    while(count < 2048) {
        const char* hit = nullptr;
        size_t klen = 0;
        for(const char* s = p; *s != 0; s++) {
            if(*s == 'i' && strncmp(s, "import", 6) == 0) { hit = s + 6; klen = 6; break; }
            if(*s == 'f' && strncmp(s, "from", 4) == 0)   { hit = s + 4; klen = 4; break; }
        }
        if(hit == nullptr) break;
        p = hit;
        const char* s = hit;
        while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        if(*s == '(') { s++; while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++; }
        if(*s == '\'' || *s == '"') {
            char q = *s++;
            const char* st = s;
            while(*s != 0 && *s != q && *s != '\n') s++;
            if(*s == q && s - st > 0 && s - st <= 512) {
                jsPrefetchEnqueue(std::string(st, (size_t)(s - st)), module_url);
                count++;
            }
            p = (*s != 0) ? s + 1 : s;
        } else {
            p = hit;
        }
        (void)klen;
    }
    if(count > 0 && getenv("EWEB_MODULE_TRACE") != nullptr)
        fprintf(stderr, "[prefetch] scan %s -> %d specifier(s)\n", module_url.c_str(), count);
}

void EWebEngine::jsPrefetchEnqueue(const std::string& spec, const std::string& base)
{
    ModulePrefetch* pf = m_jsPrefetch;
    if(pf == nullptr || spec.empty()) return;
    if(spec.compare(0, 5, "data:") == 0 || spec.compare(0, 5, "blob:") == 0)
        return;
    /* Resolve exactly like jsModuleResolve: bare specifiers only through the
     * import map, the rest against the importing module's URL. */
    std::string full;
    if(spec[0] != '.' && spec[0] != '/' && strstr(spec.c_str(), "://") == nullptr) {
        const std::string hit = jsImportMapLookup(spec.c_str());
        if(hit.empty()) return;
        full = EWebContainer::getFullURL(&m_port, hit.c_str(), jsDocumentUrl());
    } else {
        full = EWebContainer::getFullURL(&m_port, spec.c_str(), base);
    }
    if(full.empty() || strstr(full.c_str(), "://") == nullptr) return;

    pthread_mutex_lock(&pf->lock);
    pf->discovered++;
    if(pf->cache.find(full) == pf->cache.end()) {
        if(pf->cache.size() < 4096) {
            ModulePrefetch::Entry e; e.state = 0; e.status = 0; e.body = nullptr;
            pf->cache[full] = e;
            pf->queue.push_back(full);
            pthread_cond_signal(&pf->cv);
        }
    }
    pthread_mutex_unlock(&pf->lock);
}

int EWebEngine::jsPrefetchTake(const std::string& url, int* status, char** body)
{
    ModulePrefetch* pf = m_jsPrefetch;
    if(pf == nullptr) return 0;
    if(status != nullptr) *status = 0;
    if(body != nullptr) *body = nullptr;
    uint64_t t0 = ticMs();
    pthread_mutex_lock(&pf->lock);
    std::map<std::string, ModulePrefetch::Entry>::iterator it = pf->cache.find(url);
    if(it == pf->cache.end()) { pthread_mutex_unlock(&pf->lock); pf->misses++; return 0; }
    int waited = 0;
    while(it->second.state == 0 && !pf->stop) {
        waited = 1;
        pthread_cond_wait(&pf->cv, &pf->lock);
        it = pf->cache.find(url);
        if(it == pf->cache.end()) { pthread_mutex_unlock(&pf->lock); return 0; }
    }
    if(it->second.state == 1 && it->second.body != nullptr) {
        if(status != nullptr) *status = it->second.status;
        if(body != nullptr) *body = strdup(it->second.body);
        pf->hits++;
        if(waited) pf->waits++;
        uint64_t waited_ms = ticMs() - t0;
        pthread_mutex_unlock(&pf->lock);
        if(getenv("EWEB_MODULE_TRACE") != nullptr)
            fprintf(stderr, "[prefetch] HIT %s (%s, waited %llu ms)\n", url.c_str(),
                    waited ? "in-flight" : "cached", (unsigned long long)waited_ms);
        return (*body != nullptr) ? 1 : 0;
    }
    int st = it->second.status;
    pthread_mutex_unlock(&pf->lock);
    if(status != nullptr) *status = st;
    return -1;   /* worker already failed on it: don't refetch, report failure */
}

void EWebEngine::jsPrefetchStart()
{
    if(m_jsPrefetch != nullptr || js_prefetch_disabled()) return;
    int nw = 4;
    const char* e = getenv("EWEB_MODULE_PREFETCH");
    if(e != nullptr && e[0] != 0) { int v = atoi(e); if(v > 0 && v <= 16) nw = v; }
    m_jsPrefetch = new ModulePrefetch();
    for(int i = 0; i < nw; i++) {
        pthread_t t;
        /* Prefetch workers run module fetch + parse off-thread; the default
         * 512 KB pthread stack can overflow into its guard page on deep
         * bundle work. Match the task-pool headroom. */
        pthread_attr_t pattr;
        pthread_attr_init(&pattr);
        pthread_attr_setstacksize(&pattr, 8 * 1024 * 1024);
        if(pthread_create(&t, &pattr, jsPrefetchWorker, this) == 0)
            m_jsPrefetch->workers.push_back(t);
        pthread_attr_destroy(&pattr);
    }
    EWEB_LOG("[ewebview] js: module prefetch started (%d workers)\n",
             (int)m_jsPrefetch->workers.size());
}

void EWebEngine::jsPrefetchStop()
{
    ModulePrefetch* pf = m_jsPrefetch;
    if(pf == nullptr) return;
    m_jsPrefetch = nullptr;
    pthread_mutex_lock(&pf->lock);
    if(!pf->workers.empty())
        EWEB_LOG("[ewebview] js: module prefetch done (hits=%d waits=%d misses=%d "
                 "fetched=%d failed=%d discovered=%d)\n",
                 pf->hits, pf->waits, pf->misses, pf->fetched, pf->failed, pf->discovered);
    pf->stop = true;
    pthread_cond_broadcast(&pf->cv);
    pthread_mutex_unlock(&pf->lock);
    for(size_t i = 0; i < pf->workers.size(); i++)
        pthread_join(pf->workers[i], nullptr);
    for(std::map<std::string, ModulePrefetch::Entry>::iterator it = pf->cache.begin();
        it != pf->cache.end(); ++it)
        if(it->second.body != nullptr) free(it->second.body);
    pthread_mutex_destroy(&pf->lock);
    pthread_cond_destroy(&pf->cv);
    delete pf;
}

void EWebEngine::jsSeedPrefetch()
{
    if(js_prefetch_disabled() || !m_jsEnabled) return;
    jsPrefetchStart();
    if(m_jsPrefetch == nullptr) return;
    /* Seed with the parser-extracted module <script src>s; the import graph
     * fans out from there via jsPrefetchScan. */
    for(size_t i = 0; i < m_jsScriptSrcs.size(); i++) {
        if(i < m_jsScriptDeferred.size() && m_jsScriptDeferred[i] &&
           !m_jsScriptSrcs[i].empty())
            jsPrefetchEnqueue(m_jsScriptSrcs[i], jsDocumentUrl());
    }
    /* Plus every import-map target (react/react-dom/scheduler/... on github). */
    for(size_t i = 0; i < m_jsImportMap.size(); i++)
        jsPrefetchEnqueue(m_jsImportMap[i].second, jsDocumentUrl());
}

/* Import-map resolution for a bare specifier: exact key first, then the
 * longest key ending in '/' that prefixes it (package subpath form). Returns
 * the mapped address (possibly relative) or empty when unmapped. */
std::string EWebEngine::jsImportMapLookup(const char* spec)
{
    const std::string s(spec != nullptr ? spec : "");
    const std::string* best = nullptr;
    size_t best_len = 0;
    for(size_t i = 0; i < m_jsImportMap.size(); ++i) {
        const std::string& k = m_jsImportMap[i].first;
        if(k == s) { return m_jsImportMap[i].second; }
        if(!k.empty() && k[k.size()-1] == '/' && s.compare(0, k.size(), k) == 0 &&
           k.size() > best_len) { best = &m_jsImportMap[i].second; best_len = k.size(); }
    }
    return (best != nullptr) ? *best : std::string();
}

bool EWebEngine::jsRunScriptBody(const std::string& src, bool moduleish)
{
    if(getenv("EWEB_SCRIPTDBG") != nullptr)
        fprintf(stderr, "[ewebview] jsdbg: runBody moduleish=%d url=%.90s\n",
                (int)moduleish, m_jsCurScriptUrl.c_str());
    if(moduleish && !m_jsCurScriptUrl.empty()) {
        std::string abs = EWebContainer::getFullURL(&m_port, m_jsCurScriptUrl.c_str(),
                                                    jsDocumentUrl());
        if(!abs.empty())
            return vm_load_run_module(m_jsVm, src.c_str(), abs.c_str());
    }
    return vm_load_run(m_jsVm, src.c_str());
}

mstr_t* EWebEngine::jsModuleResolve(vm_t* vm, const char* spec, const char* base)
{
    EWebEngine* self = js_engine_of_vm(vm);
    if(self == nullptr || spec == nullptr || spec[0] == 0) return nullptr;
    /* Base for a relative specifier: the importing module's URL, else the
     * <script src> being run, else the document. Bare specifiers ("react")
     * resolve ONLY through the page's import map; without a hit there is no
     * browser resolution, so leave them to fail (registry key stays stable). */
    bool relative = (spec[0] == '.' || spec[0] == '/');
    bool absolute = (strstr(spec, "://") != nullptr);
    if(getenv("EWEB_SCRIPTDBG") != nullptr)
        fprintf(stderr, "[ewebview] jsdbg: importResolve spec=%.70s rel=%d curScriptUrl=%.70s vmSpec=%.70s\n",
                spec, (int)relative, self->m_jsCurScriptUrl.c_str(),
                (vm != nullptr && vm->cur_module_spec != nullptr) ? vm->cur_module_spec : "(null)");
    if(!relative && !absolute) {
        /* Import map: exact specifier first, then longest trailing-slash
         * prefix (package subpaths, e.g. "react/" + "jsx-runtime"). */
        const std::string hit = self->jsImportMapLookup(spec);
        if(hit.empty()) return nullptr;
        std::string full = EWebContainer::getFullURL(&self->m_port, hit.c_str(),
                                                     self->jsDocumentUrl());
        if(full.empty()) return nullptr;
        return mstr_new(full.c_str());
    }
    std::string b;
    if(base != nullptr && base[0] != 0) b = base;
    else if(!self->m_jsCurScriptUrl.empty())
        b = EWebContainer::getFullURL(&self->m_port, self->m_jsCurScriptUrl.c_str(),
                                      self->jsDocumentUrl());
    if(b.empty()) b = self->jsDocumentUrl();
    std::string full = EWebContainer::getFullURL(&self->m_port, spec, b);
    if(full.empty()) return nullptr;
    return mstr_new(full.c_str());
}

mstr_t* EWebEngine::jsModuleLoad(vm_t* vm, const char* spec)
{
    EWebEngine* self = js_engine_of_vm(vm);
    if(self == nullptr || spec == nullptr || spec[0] == 0) return nullptr;
    if(strstr(spec, "://") == nullptr) {
        EWEB_LOG("[ewebview] js: module '%s' has no URL resolution (bare specifier)\n", spec);
        return nullptr;
    }
    uint64_t t0 = self->ticMs();
    int status = 0;
    char* body = nullptr;
    /* Prefetch cache first: a hit costs nothing, an in-flight URL waits on
     * the worker instead of issuing a duplicate request. */
    int cached = self->m_jsPrefetch != nullptr
               ? self->jsPrefetchTake(spec, &status, &body) : 0;
    if(cached < 0) {
        EWEB_LOG("[ewebview] js: module fetch failed (%d, prefetched) %s\n", status, spec);
        return nullptr;
    }
    if(cached == 0) {
        bool ok = jsWebRequest(self, "GET", spec, "Accept: */*", nullptr, &status, &body, nullptr);
        if(!ok || body == nullptr || status < 200 || status >= 300) {
            EWEB_LOG("[ewebview] js: module fetch failed (%d) %s\n", status, spec);
            if(body != nullptr) mario_free(body);
            return nullptr;
        }
    }
    mstr_t* js = mstr_new(body);
    if(cached > 0) free(body);        /* prefetch cache hands over malloc'd */
    else mario_free(body);            /* jsWebRequest hands over mario-owned */
    EWEB_LOG("[ewebview] js: module %s -> %u bytes in %llu ms (%s)\n", spec,
             (unsigned)js->len, (unsigned long long)(self->ticMs() - t0),
             cached > 0 ? "prefetch" : "sync");
    /* The graph continues in the background even for a synchronously fetched
     * module: its own imports become prefetch candidates. */
    if(self->m_jsPrefetch != nullptr)
        self->jsPrefetchScan(spec, js->cstr);
    /* Debug aid: EWEB_MODULE_DUMP=<dir> writes every fetched module body to
     * <dir>/<n>.js (with a .url sidecar) so an in-browser compile failure can be
     * reproduced offline with `mario -c`. EWEB_MODULE_TRACE=1 logs each load. */
    const char* dumpDir = getenv("EWEB_MODULE_DUMP");
    if(dumpDir != nullptr && dumpDir[0] != 0) {
        static int dumpSeq = 0;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%d.js", dumpDir, dumpSeq);
        FILE* f = fopen(path, "wb");
        if(f != nullptr) { fwrite(js->cstr, 1, js->len, f); fclose(f); }
        snprintf(path, sizeof(path), "%s/%d.url", dumpDir, dumpSeq);
        f = fopen(path, "wb");
        if(f != nullptr) { fprintf(f, "%s\n", spec); fclose(f); }
        dumpSeq++;
    }
    if(getenv("EWEB_MODULE_TRACE") != nullptr)
        fprintf(stderr, "[module] %s -> %u bytes (status %d) in %llu ms\n", spec,
                (unsigned)js->len, status, (unsigned long long)(self->ticMs() - t0));
    /* PERMANENT WORKAROUNDS (unconditional): behaviour-neutral in a
     * spec-compliant engine, these rewrites are what lets github's file table
     * mount and commit under mario (without them the repo page stays on its SSR
     * skeleton). They stay until the underlying mario defects they mask are
     * root-caused. QADD/CROOT additionally serve as the first-commit signal for
     * jsnative's async-fetch settlement gate (see js_platform_out). */
    {
        struct ModPatch { const char* urlNeedle; const char* find; const char* repl; };
        static const ModPatch kPatches[] = {
            /* react-dom-client createWorkInProgress (rz): a fiber whose
             * alternate never ran the FiberNode ctor (tag===undefined, seen as
             * Minified #156) cannot exist in a browser; degrade the poisoned
             * alternate to the create-fresh branch (same semantics as
             * alternate===null) so the work loop survives. */
            {"react-dom-client",
             "function rz(e,n){var t=e.alternate;return null===t?(",
             "function rz(e,n){var t=e.alternate;if(null!==t&&t.tag===void 0){t=null,e.alternate=null}return null===t?("},
            /* react-core react-app hydration entry: hydrateRoot runs inside a
             * startTransition callback; a synchronous throw here would leave the
             * root half-installed and no commit would ever appear. Swallow it so
             * the transition stays consistent. */
            {"react-core",
             "let n=()=>{r.startRender(),this.#t=(0,d.hydrateRoot)(t,g,(0,m.xx)({appName:this.name}))};(0,c.startTransition)(n)",
             "let n=()=>{r.startRender();try{this.#t=(0,d.hydrateRoot)(t,g,(0,m.xx)({appName:this.name}))}catch(e){}};(0,c.startTransition)(function(){try{n()}catch(e){}})"},
            /* app-foundation zi.V: useSyncExternalStore(noop, browser, server).
             * isServer staying true means react-dom keeps serving the SERVER
             * snapshot, i.e. hydration never completed; force the browser
             * snapshot. */
            {"app-foundation",
             "function l(){return(0,n.useSyncExternalStore)(a,u,c)}",
             "function l(){return u()}"},
            /* NOYIELD: unstable_shouldYield under mario always reports true (a
             * single unit of work already exceeds the 5 ms budget), so the work
             * loop yields forever and never commits. Forcing false runs each
             * render to completion; the tree is finite and suspension is handled
             * by the surrounding try/catch, so this cannot hang. */
            {"react-dom-client",
             "eg=l.unstable_shouldYield",
             "eg=function(){return!1}"},
            /* PINGFIX: pingSuspendedRoot (sR) computes
             * `e.pingedLanes |= e.suspendedLanes & t`. markRootUpdated clears
             * suspendedLanes/pingedLanes on ANY non-idle update, so a store
             * update landing between the suspend and the wakeable's ping makes
             * the intersection 0 -> the dehydrated Suspense boundary's lane is
             * lost and it is NEVER retried. OR-ing the ping lane t
             * unconditionally preserves the retry; spurious re-pings of an
             * already-finished lane are harmless (idempotent re-render+commit). */
            {"react-dom-client",
             "e.pingedLanes|=e.suspendedLanes&t",
             "e.pingedLanes|=(e.suspendedLanes&t)|t"},
            /* commitMutationEffects' fiber walk (um per-fiber <-> ud child/
             * sibling recursion) hits a CYCLE in the fiber child/sibling lists
             * under mario: the mutual recursion runs to the run budget and the
             * watchdog cuts the run MID-COMMIT, so the data-bearing render pass
             * never reaches the DOM. A real browser never sees the cycle, so a
             * per-commit visit guard reproduces its semantics: bump a generation
             * at the commit entry, visit each fiber at most once per generation,
             * and break the sibling loop on a revisited fiber. */
            {"react-dom-client",
             "i7=ue=!1,um(n,e,t),t=cy;",
             "i7=ue=!1,um.g=(um.g||0)+1,um(n,e,t),t=cy;"},
            {"react-dom-client",
             "function um(e,n,t){var r=e.alternate,l=e.flags;",
             "function um(e,n,t){if(e.__mg===um.g)return;e.__mg=um.g;var r=e.alternate,l=e.flags;"},
            {"react-dom-client",
             "if(13886&n.subtreeFlags)for(n=n.child;null!==n;)um(n,e,t),n=n.sibling}",
             "if(13886&n.subtreeFlags)for(n=n.child;null!==n&&n.__mg!==um.g;)um(n,e,t),n=n.sibling}"},
            /* FUNCTIONAL handshake with js_platform_out (NOT a diagnostic): the
             * async-fetch settlement gate watches the console mirror for these
             * markers - QADD (react-query observer subscribe, i.e. the first
             * commit's passive flush) and CROOT (commitRoot entry, fires on
             * every commit including the hydration one). js_web_fetch_poll
             * defers async-fetch settlement until the first commit so the query
             * cache cannot flip mid-hydration (#519). The marker strings must
             * stay byte-identical to the strstr() needles in js_platform_out. */
            {"nc-",
             "addObserver(t){this.observers.includes(t)||(this.observers.push(t),this.clearGcTimeout(),",
             "addObserver(t){try{console.log('[MODP] QADD')}catch(_q){};this.observers.includes(t)||(this.observers.push(t),this.clearGcTimeout(),"},
            {"react-dom-client",
             "function ss(e,n,t){if(0!=(6&uF))throw Error(i(327));",
             "function ss(e,n,t){try{console.log('[MODP] CROOT')}catch(_){};if(0!=(6&uF))throw Error(i(327));"},
            /* RTRNEUT: neutralise the nested-Router invariant. Under mario the
             * LocationContext slot (nr._currentValue) is left dirty by an
             * earlier suspended/interrupted hydration pass whose unwind path
             * does not pop providers, so the app-root Router reads a stale
             * non-null context on the post-hydration re-render and throws - the
             * pass dies before the commit cells mount. There is only ONE Router
             * in github's tree (proven: the throwing Router is the first
             * component of its render pass - no outer Router render precedes
             * it), so the invariant is a false positive here; skip it until
             * mario's unwind/pop discipline is fixed. */
            {"react-core",
             "ro(!ay(),\"You cannot render a <Router> inside another <Router>. You should never have more than one in your app.\");",
             "void 0;"}
        };
        if(strstr(spec, "app-foundation") != nullptr) {
            std::string body(js->cstr, js->len);
            int rw = ziForceBrowserRewrite(body);
            /* gen-8 client-env: module "x" initialises n at EVAL time from
             * document.getElementById("client-env"); every deferred render
             * pass observes n unset and throws, React recovery rethrows the
             * same error, lanes drain to zero and the commit-message/date
             * columns stay skeleton forever. Let o() self-heal on first
             * call: the DOM element definitely exists by render time, so a
             * lazy re-read recovers whatever made eval-time init miss. */
            int ce = 0;
            static const char kCEnv[] = "function o(){let e=(void 0)??n;if(!e)throw";
            size_t cp = body.find(kCEnv);
            if(cp != std::string::npos) {
                body.replace(cp, sizeof(kCEnv) - 1,
                    "function o(){let e=(void 0)??n;if(!e){try{let d=document.getElementById(\"client-env\");if(d){n=JSON.parse(d.textContent||\"\");e=n}}catch(_){}}if(!e)throw");
                ce = 1;
            }
            if(getenv("EWEB_MODPATCH_DBG") != nullptr)
                fprintf(stderr, "[modpatch] ZIRW spec=%s rewrites=%d cenvlazy=%d\n", spec, rw, ce);
            if(rw > 0 || ce > 0) {
                mstr_t* nj = mstr_new(body.c_str());
                mstr_free(js);
                js = nj;
            }
        }
        /* mario closure-capture workaround (root bug is in mario's compiler:
         * a shorthand method declaring `var a` whose name collides with an
         * outer `let` that sibling arrow properties capture corrupts the
         * arrows' captured binding - ultra-minimal repro:
         *   function Q(){ let a; let r=(a=new Map,{getFile:e=>a.get(e),
         *   poison(e,t){var a;return a}}); return r; } Q().getFile("x")
         * throws "can not find function 'get' on object{}". github's
         * protobuf-registry bundle hits exactly this: getExtensionFor's
         * `var a` kills the registry's get/getFile arrows, module eval
         * throws, code-view never executes and tree-commit-info is never
         * fetched (commit-message/date columns stay skeleton forever).
         * Rename the local to break the collision until mario is fixed. */
        static const char kVarShadowFind[] =
            "getExtensionFor(e,t){var a;return null==(a=n.get(e.typeName))?void 0:a.get(t)}";
        if(strstr(js->cstr, kVarShadowFind) != nullptr) {
            std::string body(js->cstr, js->len);
            size_t vp = body.find(kVarShadowFind);
            body.replace(vp, sizeof(kVarShadowFind) - 1,
                "getExtensionFor(e,t){var z;return null==(z=n.get(e.typeName))?void 0:z.get(t)}");
            mstr_t* nj = mstr_new(body.c_str());
            mstr_free(js);
            js = nj;
            if(getenv("EWEB_MODPATCH_DBG") != nullptr)
                fprintf(stderr, "[modpatch] APPLIED varshadow spec=%s\n", spec);
        }
        for(size_t pi = 0; pi < sizeof(kPatches)/sizeof(kPatches[0]); ++pi) {
            if(strstr(spec, kPatches[pi].urlNeedle) == nullptr) continue;
            const char* hay = js->cstr;
            const char* hit = strstr(hay, kPatches[pi].find);
            if(hit == nullptr) {
                fprintf(stderr, "[modpatch] MISS needle#%d in %s\n", (int)pi, spec);
                continue;
            }
            std::string out;
            out.reserve(js->len + 512);
            out.append(hay, hit - hay);
            out.append(kPatches[pi].repl);
            out.append(hit + strlen(kPatches[pi].find));
            mstr_t* nj = mstr_new(out.c_str());
            mstr_free(js);
            js = nj;
            fprintf(stderr, "[modpatch] APPLIED needle#%d to %s\n", (int)pi, spec);
        }
    }
    return js;
}

void EWebEngine::jsSetModuleBase(bool on)
{
    if(m_jsVm == nullptr) return;
    if(!on) {
        m_jsVm->cur_module_spec = nullptr;
        return;
    }
    if(!m_jsCurScriptUrl.empty())
        m_jsModuleBase = EWebContainer::getFullURL(&m_port, m_jsCurScriptUrl.c_str(), jsDocumentUrl());
    else
        m_jsModuleBase = jsDocumentUrl();
    m_jsVm->cur_module_spec = m_jsModuleBase.empty() ? nullptr : m_jsModuleBase.c_str();
}

char* EWebEngine::jsWebGetCookie(void* ctx)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    /* The jar itself applies domain/path/Secure/SameSite matching against this
     * document's URL and filters HttpOnly entries out, so what comes back is
     * already the "k=v; k2=v2" string document.cookie reports. A document with
     * no URL (loadHtmlContent) gets an empty jar view, as an about:blank does. */
    std::string view = EWebCookieJar::instance().jsGet(self->jsDocumentUrl());
    /* Env-gated: an empty document.cookie where the request Cookie header has
     * entries means the jar filed them as HttpOnly; print the JS view to tell
     * the two apart when a server-side CSRF check fails. */
    if(getenv("EWEB_NETDBG") != NULL)
        fprintf(stderr, "[netdbg] document.cookie=%s\n",
                view.empty() ? "(empty)" : view.c_str());
    return js_strdup_mario(view.c_str());
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
    cb.get_color_scheme = jsWebGetColorScheme;

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
    cb.update_url   = jsWebUpdateUrl;

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
    /* Async fetch worker: same implementation (jsWebRequestRaw is thread-safe),
     * called from a detached pthread spawned by js_web.c's native_fetch. */
    cb.http_request_worker = jsWebRequest;

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

void EWebEngine::jsRestoreNoJsFallback()
{
    /* After all scripts have run, check whether the `no-js` progressive
     * enhancement class should be restored on <html>. The original markup
     * carries it (`<html class="no-js ...">`) and a tiny inline handler
     * immediately strips it, assuming the full JS enhancement pipeline (React
     * hydration, GSAP tweens, IntersectionObserver callbacks) will run and
     * eventually toggle content from opacity:0 to 1. When the engine cannot
     * complete the async hydration (e.g. Next.js initialize() stalls because
     * the mario VM doesn't settle its internal Promises), the class never
     * transitions to "enhanced" and the SSR content stays invisible behind
     * the tween's initial opacity:0. Restoring `no-js` reactivates the
     * fallback CSS that shows content at full opacity, matching what a
     * no-JavaScript browser would display. */
    if(!m_jsOrigHtmlHadNoJs) return;
    litehtml::document* doc = jsActiveDoc();
    if(doc == nullptr) return;
    litehtml::element::ptr root = doc->root();
    if(root == nullptr) return;
    litehtml::element::ptr html_el = root->select_one("html");
    if(html_el == nullptr) html_el = root; // root IS <html> in most trees
    const char* cls = html_el->get_attr("class", "");
    std::string clsStr = cls ? cls : "";
    /* If the full enhancement pipeline ran (added "enhanced"), don't override. */
    if(clsStr.find("enhanced") != std::string::npos) return;
    /* Already has no-js (handler never ran): nothing to do. */
    if(clsStr.find("no-js") != std::string::npos) return;
    /* Re-add no-js. */
    if(!clsStr.empty()) clsStr += " ";
    clsStr += "no-js";
    html_el->set_attr("class", clsStr.c_str());
    EWEB_LOG("[ewebview] restored no-js fallback class on <html>\n");
}

void EWebEngine::jsFireLoadEvents()
{
    if(m_jsVm == nullptr || !m_jsEnabled || m_jsPageDisabled || m_jsInScript) return;
    /* Idempotent: the streaming-SSR early-fire (runNextPageScript) and the
     * phase-completion path (ewebview.cc) can both reach here; load must fire
     * exactly once per navigation. */
    if(m_jsLoadFired) return;
    m_jsLoadFired = true;
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
             * because that is what position:fixed elements are tested against.
             * Note the scroll first so sticky boxes test where they pin. */
            doc->set_hit_scroll(m_engineScrollX, m_engineScrollY);
            target = root->get_element_by_point(cx + m_engineScrollX, cy + m_engineScrollY, cx, cy);
        }
        if(target == nullptr && root != nullptr) target = root;   /* blank area: the root element */
    }
    if(getenv("EWEB_HITDBG") != nullptr && target != nullptr) {
        litehtml::position hb = target->get_placement();
        fprintf(stderr, "[hitdbg] cx=%d cy=%d tag=%s id=%s cls=%.80s box=%d,%d %dx%d disp=%d posn=%d vis=%d nkids=%d doc=%p mdoc=%p bdoc=%p\n", cx, cy,
                target->get_tagName(), target->get_attr("id", ""),
                target->get_attr("class", ""), hb.x, hb.y, hb.width, hb.height,
                (int)target->get_display(), (int)target->get_element_position(),
                (int)target->is_visible(), (int)target->get_children_count(),
                (void*)doc, (void*)m_doc, (void*)m_buildDoc);
        /* Ancestor chain + child boxes in ABSOLUTE placement coords so a
         * degenerate wrapper / offset descent can be seen unambiguously. */
        for(litehtml::element* a = target->parent(); a != nullptr && a != (litehtml::element*)doc->root(); a = a->parent()) {
            litehtml::position ap = a->get_placement();
            fprintf(stderr, "[hitdbg-anc] tag=%s box=%d,%d %dx%d disp=%d nkids=%d\n", a->get_tagName(),
                    ap.x, ap.y, ap.width, ap.height, (int)a->get_display(), (int)a->get_children_count());
        }
        for(int k = 0; k < (int)target->get_children_count() && k < 8; k++) {
            litehtml::element::ptr c = target->get_child(k);
            if(c == nullptr) continue;
            litehtml::position cp = c->get_placement();
            fprintf(stderr, "[hitdbg-kid] k=%d tag=%s cls=%.40s box=%d,%d %dx%d disp=%d vis=%d inside=%d\n", k,
                    c->get_tagName(), c->get_attr("class", ""), cp.x, cp.y, cp.width, cp.height,
                    (int)c->get_display(), (int)c->is_visible(), (int)c->is_point_inside(cx, cy));
        }
    }
    /* EWEB_DOMSIG: a click lands well after hydration; dump the live tree once
     * here so a post-hydration collapse (opacity/height/position) is visible. */
    if(getenv("EWEB_DOMSIG") != nullptr && doc != nullptr && doc->root() != nullptr) {
        static bool s_domsigOnClick = false;
        if(!s_domsigOnClick) {
            s_domsigOnClick = true;
            litehtml::element* cont = (litehtml::element*)doc->root()->select_one("#__PWS_ROOT__");
            fprintf(stderr, "[domsig] === ON-CLICK live #__PWS_ROOT__ (cont=%d) ===\n", cont != nullptr);
            ewebDumpDomSig(cont != nullptr ? cont : (litehtml::element*)doc->root(), 0);
        }
    }

    if(target != nullptr) {
        /* m_jsInScript arms the VM interrupt hook for the handlers' duration:
         * a heavy onclick/onmousemove listener stays interruptible by a
         * termination request (m_buildAbort) instead of pinning the engine. */
        jsVmEnter();
        if(track_hover && target != (litehtml::element*)m_jsHoverElement) {
            litehtml::element* prev = (litehtml::element*)m_jsHoverElement;
            m_jsHoverElement = (void*)target;
            /* Drive CSS :hover as well as the DOM mouseover/mouseout events.
             * applyPseudoClassChain() toggles "hover" on the element and every
             * ancestor and re-runs the cascade, so :hover rules actually
             * repaint. Order: clear the previous chain first, then set the new
             * one, so shared ancestors don't briefly lose :hover (they'd just
             * be re-added, but clearing first keeps the restyle walk cheap).
             * Skip the whole block when the VM is off since m_jsHoverElement
             * is only meaningful inside a JS-aware dispatch. */
            applyPseudoClassChain(prev, "hover", false);
            applyPseudoClassChain(target, "hover", true);
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
