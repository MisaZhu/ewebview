/* EWebCanvasGlue.cc - the Canvas 2D embedder half of ewebview.
 *
 * The glue between the pure-C bridge (mario natives/js_canvas.c) and the rest
 * of the engine:
 *
 *   - Thin C-callback trampolines. The bridge calls through
 *     js_canvas_callbacks_t function pointers; each trampoline just casts the
 *     opaque handle back to an EWebCanvas* and forwards to the matching
 *     method. All the actual drawing lives in the EWebCanvas class
 *     (EWebCanvas.h/.cc), which maps 1:1 onto the port's gfx/font tables -
 *     nothing is re-implemented here.
 *   - The two callbacks that need engine internals (canvas_create,
 *     bitmap_from_element) stay static members so they can reach the canvas
 *     registry and the active EWebContainer.
 *   - The canvas registry itself: getOrCreateCanvas / freeCanvases /
 *     compositeCanvases / registerCanvasNatives.
 *
 * Threading: every callback runs synchronously on the engine thread inside a
 * VM run; the engine exclusively owns the VM and the documents, so no locking
 * is needed (and none may be taken). `ctx` is the owning EWebEngine; `canvas`
 * is the EWebCanvas* that jsCanvasCreate handed back. The EWebCanvas objects
 * are owned by the engine and freed per page load in cleanupBuildResources();
 * the bridge's per-context state is freed by vm_close().
 */

#include "EWebInternal.h"
#include "EWebLog.h"

#include <mario/js_canvas.h>

#include <string>
#include <string.h>

namespace eweb {

/* ==================================================================
 * C-callback trampolines -> EWebCanvas methods
 *
 * File-static (not EWebEngine members): they only need the EWebCanvas and the
 * port, never the engine's private state. `canvas` is the EWebCanvas* the
 * bridge stored; `ctx` is the EWebEngine (used only to reach the port).
 * ================================================================== */

static void jsCanvasCanvasDims(void* ctx, void* canvas, int* w, int* h)
{
    (void)ctx;
    EWebCanvas* cv = (EWebCanvas*)canvas;
    if(w) *w = (cv != nullptr) ? cv->width()  : 0;
    if(h) *h = (cv != nullptr) ? cv->height() : 0;
}

/* ---- Anonymous offscreen bitmaps (ImageData / pattern / drawImage src) ----
 * These are raw eweb_surface_t* the BRIDGE owns, not EWebCanvas objects, so
 * they go straight to the port's gfx table. Cleared to transparent so
 * putImageData / getImageData see a defined buffer; the surface is uint32_t
 * ARGB, matching the bridge's pixel format. */

static void* jsCanvasBitmapCreate(void* ctx, int w, int h)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || w <= 0 || h <= 0) return nullptr;
    const eweb_gfx_t& gfx = self->m_port.gfx;
    eweb_surface_t* g = gfx.surface_new(gfx.ud, w, h);
    if(g == nullptr) return nullptr;
    if(gfx.surface_clear != nullptr)
        gfx.surface_clear(gfx.ud, g, 0x00000000u);   /* transparent black */
    return (void*)g;
}

static void jsCanvasBitmapFree(void* ctx, void* bitmap)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || bitmap == nullptr) return;
    self->m_port.gfx.surface_free(self->m_port.gfx.ud, (eweb_surface_t*)bitmap);
}

static void jsCanvasBitmapDims(void* ctx, void* bitmap, int* w, int* h)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(w) *w = 0;
    if(h) *h = 0;
    if(self == nullptr || bitmap == nullptr) return;
    const eweb_gfx_t& gfx = self->m_port.gfx;
    if(gfx.surface_dims != nullptr)
        gfx.surface_dims(gfx.ud, (eweb_surface_t*)bitmap, w, h);
}

static uint32_t* jsCanvasBitmapData(void* ctx, void* bitmap, int* w, int* h)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(w) *w = 0;
    if(h) *h = 0;
    if(self == nullptr || bitmap == nullptr) return nullptr;
    const eweb_gfx_t& gfx = self->m_port.gfx;
    if(gfx.surface_pixels == nullptr) return nullptr;   /* bridge falls back to get/set_pixel */
    return gfx.surface_pixels(gfx.ud, (eweb_surface_t*)bitmap, w, h);
}

/* ---- Drawing primitives -> EWebCanvas methods ---- */

static void jsCanvasFillRect(void* ctx, void* canvas, int x, int y, int w, int h, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->fillRect(x, y, w, h, color);
}

static void jsCanvasStrokeRect(void* ctx, void* canvas, int x, int y, int w, int h, int lw, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->strokeRect(x, y, w, h, lw, color);
}

static void jsCanvasDrawLine(void* ctx, void* canvas, int x0, int y0, int x1, int y1, int w, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->drawLine(x0, y0, x1, y1, w, color);
}

static void jsCanvasFillCircle(void* ctx, void* canvas, int cx, int cy, int r, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->fillCircle(cx, cy, r, color);
}

static void jsCanvasStrokeCircle(void* ctx, void* canvas, int cx, int cy, int r, int lw, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->strokeCircle(cx, cy, r, lw, color);
}

static void jsCanvasFillArc(void* ctx, void* canvas, int cx, int cy, int r, float a0, float a1, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->fillArc(cx, cy, r, a0, a1, color);
}

static void jsCanvasStrokeArc(void* ctx, void* canvas, int cx, int cy, int r, int lw, float a0, float a1, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->strokeArc(cx, cy, r, lw, a0, a1, color);
}

static void jsCanvasFillRoundRect(void* ctx, void* canvas, int x, int y, int w, int h, int r, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->fillRoundRect(x, y, w, h, r, color);
}

static void jsCanvasStrokeRoundRect(void* ctx, void* canvas, int x, int y, int w, int h, int r, int lw, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->strokeRoundRect(x, y, w, h, r, lw, color);
}

static void jsCanvasStrokeQuadratic(void* ctx, void* canvas, int x0, int y0, int cx, int cy, int x1, int y1, int lw, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->strokeQuadratic(x0, y0, cx, cy, x1, y1, lw, color);
}

static void jsCanvasStrokeBezier(void* ctx, void* canvas, int x0, int y0, int cx1, int cy1, int cx2, int cy2, int x1, int y1, int lw, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->strokeBezier(x0, y0, cx1, cy1, cx2, cy2, x1, y1, lw, color);
}

/* ---- Curve flattening (pure math, no bitmap) ----
 * The bridge calls these to bake a curve into its device-space path so it can
 * scanline FILL it (the gfx curve strokers only draw). Forward to the port's
 * flatteners so the de Casteljau subdivision lives in exactly one place; the
 * HAL marks them OPTIONAL, so a missing hook degrades to a straight chord. */

static int jsCanvasFlattenQuadratic(void* ctx, float x0, float y0, float cx, float cy, float x1, float y1, float* xy, int max_pts)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self != nullptr && self->m_port.gfx.flatten_quadratic != nullptr)
        return self->m_port.gfx.flatten_quadratic(self->m_port.gfx.ud,
                                                  x0, y0, cx, cy, x1, y1, xy, max_pts);
    /* Chord fallback: the end point only (start excluded per the contract). */
    if(xy == nullptr || max_pts < 1) return 0;
    xy[0] = x1;
    xy[1] = y1;
    return 1;
}

static int jsCanvasFlattenCubic(void* ctx, float x0, float y0, float cx1, float cy1, float cx2, float cy2, float x1, float y1, float* xy, int max_pts)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self != nullptr && self->m_port.gfx.flatten_cubic != nullptr)
        return self->m_port.gfx.flatten_cubic(self->m_port.gfx.ud,
                                              x0, y0, cx1, cy1, cx2, cy2, x1, y1, xy, max_pts);
    if(xy == nullptr || max_pts < 1) return 0;
    xy[0] = x1;
    xy[1] = y1;
    return 1;
}

/* ---- Pixel access / blit ---- */

static void jsCanvasSetPixel(void* ctx, void* canvas, int x, int y, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->setPixel(x, y, color);
}

static uint32_t jsCanvasGetPixel(void* ctx, void* canvas, int x, int y)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    return cv ? cv->getPixel(x, y) : 0;
}

static void jsCanvasBlit(void* ctx, void* canvas, void* src,
                         int sx, int sy, int sw, int sh,
                         int dx, int dy, int dw, int dh,
                         uint8_t alpha, int smooth)
{
    (void)ctx; (void)smooth;
    EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->blit((eweb_surface_t*)src, sx, sy, sw, sh, dx, dy, dw, dh, alpha);
}

/* ---- Text ---- */

static void jsCanvasDrawText(void* ctx, void* canvas, int x, int y, const char* text, float size, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->drawText(x, y, text, size, color);
}

static void jsCanvasStrokeText(void* ctx, void* canvas, int x, int y, const char* text, float size, int lw, uint32_t color)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->strokeText(x, y, text, size, lw, color);
}

static void jsCanvasTextSize(void* ctx, void* canvas, const char* text, float size, int* w, int* h)
{
    (void)ctx;
    EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv == nullptr) { if(w) *w = 0; if(h) *h = 0; return; }
    cv->textSize(text, size, w, h);
}

/* ---- Clipping ---- */

static void jsCanvasSetClip(void* ctx, void* canvas, int x, int y, int w, int h)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->setClip(x, y, w, h);
}

static void jsCanvasClearClip(void* ctx, void* canvas)
{
    (void)ctx; EWebCanvas* cv = (EWebCanvas*)canvas;
    if(cv) cv->clearClip();
}

/* ==================================================================
 * Callbacks that need EWebEngine internals (static members)
 * ================================================================== */

/* canvas_create: look up (or lazily create) the EWebCanvas backing the
 * <canvas> element whose id the bridge passes. Needs the engine's registry. */
void* EWebEngine::jsCanvasCreate(void* ctx, const char* id, int w, int h)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr) return nullptr;
    return (void*)self->getOrCreateCanvas((id != nullptr) ? id : "", w, h);
}

/* Resolve an <img> Element handle to its decoded bitmap. The element handle is
 * the litehtml::element* the DOM bridge stashed on the Element instance's
 * ->value; read its src attr, resolve against the container's base URL, and
 * look it up in the same cache litehtml uses for layout. The returned
 * eweb_surface_t* is owned by the EWebContainer - the bridge must NOT free it.
 * Needs the engine's active container, so it stays a member. */
void* EWebEngine::jsCanvasBitmapFromElement(void* ctx, void* el)
{
    EWebEngine* self = (EWebEngine*)ctx;
    if(self == nullptr || el == nullptr) return nullptr;
    litehtml::element* e = (litehtml::element*)el;
    const char* src = e->get_attr("src", nullptr);
    if(src == nullptr || src[0] == 0) return nullptr;
    /* Pick the container that owns the element: build container while a build
     * is in flight (scripts run in BUILD_RUN_JS), else the visible container. */
    EWebContainer* container = (self->m_buildContainer != nullptr) ? self->m_buildContainer : self->m_container;
    if(container == nullptr) return nullptr;
    std::string url = container->resolveUrl(std::string(src));
    return (void*)container->getImage(url);
}

/* ==================================================================
 * EWebEngine canvas registry
 * ================================================================== */

EWebCanvas* EWebEngine::getOrCreateCanvas(const std::string& id, int w, int h)
{
    for(size_t i = 0; i < m_jsCanvases.size(); ++i) {
        if(m_jsCanvases[i] != nullptr && m_jsCanvases[i]->id() == id)
            return m_jsCanvases[i];
    }
    EWebCanvas* cv = new EWebCanvas(&m_port, id, w, h);   /* ctor clamps to the 300x150 default */
    if(!cv->valid()) { delete cv; return nullptr; }
    m_jsCanvases.push_back(cv);
    return cv;
}

void EWebEngine::freeCanvases()
{
    for(size_t i = 0; i < m_jsCanvases.size(); ++i)
        delete m_jsCanvases[i];   /* ~EWebCanvas frees the bitmap + font */
    m_jsCanvases.clear();
}

/* Blit every live <canvas> backing store over the laid-out page at the
 * element's placement. Synthetic ids (leading '@', used for offscreen
 * ImageData/pattern canvases) have no element and are skipped. */
void EWebEngine::compositeCanvases(eweb_surface_t* g, int ox, int oy)
{
    if(g == nullptr || m_jsCanvases.empty() || m_doc == nullptr) return;
    if(m_port.gfx.blit == nullptr) return;
    litehtml::element::ptr root = m_doc->root();
    if(root == nullptr) return;
    for(size_t i = 0; i < m_jsCanvases.size(); ++i) {
        EWebCanvas* cv = m_jsCanvases[i];
        if(cv == nullptr || cv->bitmap() == nullptr) continue;
        const std::string& cid = cv->id();
        if(cid.empty() || cid[0] == '@') continue;   /* synthetic key: no element */
        std::string sel = std::string("#") + cid;
        litehtml::element::ptr el = root->select_one(sel.c_str());
        if(el == nullptr) continue;
        litehtml::position p = el->get_placement();
        m_port.gfx.blit(m_port.gfx.ud, cv->bitmap(), 0, 0, cv->width(), cv->height(),
                        g, ox + p.x, oy + p.y, cv->width(), cv->height());
    }
}

void EWebEngine::registerCanvasNatives(struct st_vm* vm)
{
    if(vm == nullptr) return;

    /* Hand the pure-C bridge (natives/js_canvas.c) the platform callbacks; it
     * registers the CanvasRenderingContext2D class plus getContext()/width/
     * height on the Element class js_dom.c already created. el_get_attr reuses
     * the DOM bridge's attribute reader verbatim.
     *
     * fill_polygon / stroke_polygon are left NULL on purpose: the bridge's
     * scanline fill / polyline stroke path via fill_rect/draw_line is the same
     * work the port would do, and not implementing them keeps the dispatch
     * surface minimal. */
    js_canvas_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    /* Lifecycle (static members: need the engine registry / container) */
    cb.canvas_create         = jsCanvasCreate;
    cb.canvas_dims           = jsCanvasCanvasDims;
    cb.el_get_attr           = jsElGetAttr;
    cb.bitmap_from_element   = jsCanvasBitmapFromElement;
    /* Anonymous offscreen bitmaps (raw eweb_surface_t*) */
    cb.bitmap_create         = jsCanvasBitmapCreate;
    cb.bitmap_free           = jsCanvasBitmapFree;
    cb.bitmap_dims           = jsCanvasBitmapDims;
    cb.bitmap_data           = jsCanvasBitmapData;
    /* Drawing primitives -> EWebCanvas methods (port gfx/font tables) */
    cb.fill_rect             = jsCanvasFillRect;
    cb.stroke_rect           = jsCanvasStrokeRect;
    cb.draw_line             = jsCanvasDrawLine;
    cb.fill_circle           = jsCanvasFillCircle;
    cb.stroke_circle         = jsCanvasStrokeCircle;
    cb.fill_arc              = jsCanvasFillArc;
    cb.stroke_arc            = jsCanvasStrokeArc;
    cb.fill_round_rect       = jsCanvasFillRoundRect;
    cb.stroke_round_rect     = jsCanvasStrokeRoundRect;
    /* Curves: stroke via the port's strokers; flatten via the port's
     * flatteners so the bridge's fill path reuses the same de Casteljau
     * subdivision instead of duplicating it. */
    cb.stroke_quadratic      = jsCanvasStrokeQuadratic;
    cb.stroke_bezier         = jsCanvasStrokeBezier;
    cb.flatten_quadratic     = jsCanvasFlattenQuadratic;
    cb.flatten_cubic         = jsCanvasFlattenCubic;
    /* Polygon ops: left NULL (bridge handles via fill_rect/draw_line). */
    /* Pixel access */
    cb.set_pixel             = jsCanvasSetPixel;
    cb.get_pixel             = jsCanvasGetPixel;
    /* Blit */
    cb.blit                  = jsCanvasBlit;
    /* Text */
    cb.draw_text             = jsCanvasDrawText;
    cb.stroke_text           = jsCanvasStrokeText;
    cb.text_size             = jsCanvasTextSize;
    /* Clipping */
    cb.set_clip              = jsCanvasSetClip;
    cb.clear_clip            = jsCanvasClearClip;

    if(!js_register_canvas_natives(vm, this, &cb)) {
        EWEB_LOG("[ewebview] js: canvas native registration failed\n");
    }
}

} /* namespace eweb */
