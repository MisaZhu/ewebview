/*
 * js_canvas.h - HTML5 Canvas 2D natives for the mario JavaScript VM.
 *
 * This is a pure C bridge, mirroring js_dom.h: it knows nothing about
 * WidgetWebview, litehtml, graph_t, or font_t. All the CanvasRenderingContext2D
 * state (current path, style, CTM, save stack, clip, dash, gradient, shadow)
 * and geometry (arc/ellipse/bezier tessellation, scanline fill, polyline
 * stroke, gradient spans, pattern tiling) lives here; the embedding application
 * supplies a js_canvas_callbacks_t whose function pointers push the actual
 * pixels into an opaque backing store the embedder owns.
 *
 * The callback surface is intentionally thin and maps 1:1 onto the EwokOS
 * graph library primitives (graph_fill_rect / graph_wline / graph_circle /
 * graph_arc / graph_fill_round / graph_blt_fit_alpha / graph_pixel /
 * graph_set_clip ...). The bridge never re-implements a primitive the graph
 * library already provides; it only adds the geometry the graph library does
 * not have (polygon scanline fill, bezier tessellation, gradient span math,
 * pattern tiling, dash segmentation, shadow offsets) and dispatches the result
 * through these callbacks.
 *
 * Relationship to js_dom.h:
 *   - js_register_canvas_natives() must be called AFTER js_register_dom_natives()
 *     because it hangs getContext()/width/height onto the "Element" class the
 *     DOM bridge already created, and reuses the Element instances' ->value
 *     handle.
 *   - The element handle carried by an Element instance is opaque here (void*);
 *     it is handed straight back to el_get_attr / bitmap_from_element, which
 *     may reuse the DOM bridge's callbacks verbatim.
 *
 * Lifetime / threading contract:
 *   - All callbacks run synchronously on the VM thread, exactly like the DOM
 *     bridge. The embedder is responsible for any locking.
 *   - canvas_create() returns an opaque canvas handle the embedder owns (e.g.
 *     a JsCanvas* / graph_t bitmap). The bridge never frees it; it only passes
 *     it back into the drawing callbacks. The embedder tears canvases down
 *     with the page, after vm_close() has freed this bridge's per-context
 *     state.
 *   - bitmap_create() returns an anonymous offscreen bitmap the BRIDGE owns
 *     for the lifetime of an ImageData / CanvasPattern / drawImage source. The
 *     embedder must free it via bitmap_free() when the bridge calls it.
 *   - el_get_attr() returns a mario_malloc'd NUL-terminated string (or NULL);
 *     the natives free it after parsing. It must NOT return a string literal
 *     or a stack buffer.
 *
 * Supported surface (full CanvasRenderingContext2D per the WHATWG spec, minus
 * the parts that require a real DOM/HTMLImageElement lifecycle):
 *   canvas.getContext('2d')            - on Element, returns a context or null
 *   canvas.width / canvas.height       - on Element, from the markup attributes
 *
 *   State:        save, restore, reset
 *   Transform:    scale, rotate, translate, transform, setTransform,
 *                 getTransform, resetTransform
 *   Compositing:  globalAlpha, globalCompositeOperation
 *   Image:        imageSmoothingEnabled, imageSmoothingQuality
 *   Fill/stroke:  fillStyle, strokeStyle (color | CanvasGradient | CanvasPattern)
 *   Line:         lineWidth, lineCap, lineJoin, miterLimit,
 *                 lineDashOffset, setLineDash, getLineDash
 *   Shadow:       shadowBlur, shadowColor, shadowOffsetX, shadowOffsetY
 *   Filter:       filter (accepted, currently ignored)
 *   Path:         beginPath, closePath, moveTo, lineTo,
 *                 quadraticCurveTo, bezierCurveTo, arcTo,
 *                 rect, roundRect, arc, ellipse
 *   Draw path:    fill, stroke, clip, isPointInPath, isPointInStroke
 *   Draw rect:    clearRect, fillRect, strokeRect
 *   Draw text:    fillText, strokeText, measureText
 *   Text style:   font, textAlign, textBaseline, direction,
 *                 letterSpacing, wordSpacing, fontKerning, fontStretch,
 *                 fontVariantCaps, textRendering
 *   Draw image:   drawImage (3/5/9-arg forms)
 *   Pixel:        createImageData, getImageData, putImageData
 *   Gradient:     createLinearGradient, createRadialGradient,
 *                 createConicGradient  -> CanvasGradient.addColorStop
 *   Pattern:      createPattern        -> CanvasPattern.setTransform
 *   Misc:         canvas (read-only), getContextAttributes
 *   Path2D:       constructor + all path methods + addPath
 */

#ifndef MARIO_JS_CANVAS_H
#define MARIO_JS_CANVAS_H

#include "mario.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Composite operation enum (matches the spec strings; the bridge stores the
 * int and hands it to the embedder, which may ignore unsupported modes). */
enum {
    JS_GCO_SOURCE_OVER = 0,
    JS_GCO_SOURCE_IN,
    JS_GCO_SOURCE_OUT,
    JS_GCO_SOURCE_ATOP,
    JS_GCO_DESTINATION_OVER,
    JS_GCO_DESTINATION_IN,
    JS_GCO_DESTINATION_OUT,
    JS_GCO_DESTINATION_ATOP,
    JS_GCO_LIGHTER,
    JS_GCO_COPY,
    JS_GCO_XOR,
    JS_GCO_MULTIPLY,
    JS_GCO_SCREEN,
    JS_GCO_OVERLAY,
    JS_GCO_DARKEN,
    JS_GCO_LIGHTEN,
    JS_GCO_COLOR_DODGE,
    JS_GCO_COLOR_BURN,
    JS_GCO_HARD_LIGHT,
    JS_GCO_SOFT_LIGHT,
    JS_GCO_DIFFERENCE,
    JS_GCO_EXCLUSION,
    JS_GCO_HUE,
    JS_GCO_SATURATION,
    JS_GCO_COLOR,
    JS_GCO_LUMINOSITY
};

/* Line cap / join enums. */
enum {
    JS_LINE_CAP_BUTT = 0,
    JS_LINE_CAP_ROUND,
    JS_LINE_CAP_SQUARE
};
enum {
    JS_LINE_JOIN_MITER = 0,
    JS_LINE_JOIN_ROUND,
    JS_LINE_JOIN_BEVEL
};

/* Text align / baseline enums. */
enum {
    JS_TEXT_ALIGN_LEFT = 0,
    JS_TEXT_ALIGN_CENTER,
    JS_TEXT_ALIGN_RIGHT,
    JS_TEXT_ALIGN_START,
    JS_TEXT_ALIGN_END
};
enum {
    JS_TEXT_BASELINE_TOP = 0,
    JS_TEXT_BASELINE_HANGING,
    JS_TEXT_BASELINE_MIDDLE,
    JS_TEXT_BASELINE_ALPHABETIC,
    JS_TEXT_BASELINE_IDEOGRAPHIC,
    JS_TEXT_BASELINE_BOTTOM
};

/* Image smoothing quality. */
enum {
    JS_SMOOTH_LOW = 0,
    JS_SMOOTH_MEDIUM,
    JS_SMOOTH_HIGH
};

/* Pattern repeat modes. */
enum {
    JS_PATTERN_REPEAT = 0,
    JS_PATTERN_REPEAT_X,
    JS_PATTERN_REPEAT_Y,
    JS_PATTERN_NO_REPEAT
};

/* Gradient types. */
enum {
    JS_GRAD_LINEAR = 0,
    JS_GRAD_RADIAL,
    JS_GRAD_CONIC
};

typedef struct js_canvas_callbacks {
    /* ---- Lifecycle ---------------------------------------------------- */

    /* Get or create the backing store for a <canvas> keyed by `id` at w x h.
     * Returns an opaque handle the embedder owns (NULL on failure -> JS null).
     * Called from getContext(); the same id must yield the same handle so the
     * context state (path/style) stays bound to one bitmap. */
    void* (*canvas_create)(void* ctx, const char* id, int w, int h);

    /* Query the dimensions of a canvas handle returned by canvas_create.
     * Used by getImageData / createImageData / drawImage(canvas) to size
     * buffers without re-reading the markup attributes. */
    void  (*canvas_dims)(void* ctx, void* canvas, int* w, int* h);

    /* Read an attribute off an Element handle (id/width/height/src/...).
     * mario_malloc'd string or NULL. May reuse the DOM bridge's el_get_attr. */
    char* (*el_get_attr)(void* ctx, void* el, const char* name);

    /* ---- Anonymous offscreen bitmaps ----------------------------------
     * Used for ImageData backing stores, CanvasPattern sources, and as the
     * source side of drawImage(otherCanvas). The BRIDGE owns the lifetime:
     * it calls bitmap_create, hands the opaque pointer back to the embedder
     * via blit / get_pixel / set_pixel, and eventually calls bitmap_free.
     * The embedder may implement these as graph_new / graph_free. */
    void* (*bitmap_create)(void* ctx, int w, int h);
    void  (*bitmap_free)(void* ctx, void* bitmap);
    void  (*bitmap_dims)(void* ctx, void* bitmap, int* w, int* h);
    /* Direct ARGB pixel access, w*h uint32_t. May return NULL if the embedder
     * cannot expose the buffer; the bridge then falls back to get/set_pixel. */
    uint32_t* (*bitmap_data)(void* ctx, void* bitmap, int* w, int* h);

    /* Resolve an <img> Element handle to its decoded bitmap. Returns an
     * opaque handle the embedder owns (NULL if not loaded / not an image).
     * The bridge does NOT free this; it only passes it back into blit. */
    void* (*bitmap_from_element)(void* ctx, void* el);

    /* ---- Basic pixel ops (map 1:1 onto graph library) ----------------- */
    /* All coordinates are device space (the CTM was already applied by the
     * natives); the embedder clips to the bitmap. Colors are 0xAARRGGBB. */
    void (*fill_rect)(void* ctx, void* canvas, int x, int y, int w, int h, uint32_t color);
    void (*stroke_rect)(void* ctx, void* canvas, int x, int y, int w, int h, int lw, uint32_t color);
    void (*draw_line)(void* ctx, void* canvas, int x0, int y0, int x1, int y1, int lw, uint32_t color);
    void (*fill_circle)(void* ctx, void* canvas, int cx, int cy, int r, uint32_t color);
    void (*stroke_circle)(void* ctx, void* canvas, int cx, int cy, int r, int lw, uint32_t color);
    /* Angles in radians, measured clockwise from +x (canvas convention). */
    void (*fill_arc)(void* ctx, void* canvas, int cx, int cy, int r, float a0, float a1, uint32_t color);
    void (*stroke_arc)(void* ctx, void* canvas, int cx, int cy, int r, int lw, float a0, float a1, uint32_t color);
    void (*fill_round_rect)(void* ctx, void* canvas, int x, int y, int w, int h, int r, uint32_t color);
    void (*stroke_round_rect)(void* ctx, void* canvas, int x, int y, int w, int h, int r, int lw, uint32_t color);

    /* ---- Polygon ops (device space, float coords) ---------------------
     * The graph library has no general polygon primitive, so the bridge
     * offers the embedder the raw vertex list and lets it decide (CPU
     * scanline, GPU tessellation, ...). If NULL, the bridge falls back to
     * its own scanline fill via fill_rect spans. */
    void (*fill_polygon)(void* ctx, void* canvas, const float* xy, int n, uint32_t color);
    void (*stroke_polygon)(void* ctx, void* canvas, const float* xy, int n,
                           int closed, int lw, int cap, int join, uint32_t color);

    /* ---- Curve ops (map onto graph/curve.h) ---------------------------
     * The graph library already flattens quadratic/cubic beziers (adaptive
     * de Casteljau) and strokes them with graph_line/graph_wline, so the
     * bridge must NOT re-implement that subdivision.
     *   stroke_quadratic / stroke_bezier -> graph_quadratic_curve_w /
     *                                       graph_bezier_curve_w. Used when a
     *                                       subpath is exactly one curve; all
     *                                       coords are DEVICE space ints.
     *   flatten_quadratic / flatten_cubic -> graph_flatten_quadratic /
     *                                       graph_flatten_cubic. Write the
     *                                       flattened polyline into `xy` as
     *                                       interleaved x,y floats (up to
     *                                       max_pts vertices; the start point
     *                                       is excluded, matching the graph
     *                                       contract) and return the vertex
     *                                       count. The bridge calls these to
     *                                       bake a curve into its device-space
     *                                       path so it can scanline FILL it
     *                                       (the graph curve funcs only stroke)
     *                                       without duplicating the math.
     *                                       Coords are in whatever space the
     *                                       bridge passes (user space; it
     *                                       transforms the result). */
    void (*stroke_quadratic)(void* ctx, void* canvas, int x0, int y0,
                             int cx, int cy, int x1, int y1, int lw, uint32_t color);
    void (*stroke_bezier)(void* ctx, void* canvas, int x0, int y0,
                          int cx1, int cy1, int cx2, int cy2, int x1, int y1,
                          int lw, uint32_t color);
    int  (*flatten_quadratic)(void* ctx, float x0, float y0, float cx, float cy,
                              float x1, float y1, float* xy, int max_pts);
    int  (*flatten_cubic)(void* ctx, float x0, float y0, float cx1, float cy1,
                          float cx2, float cy2, float x1, float y1,
                          float* xy, int max_pts);

    /* ---- Pixel access (for ImageData / readback) ---------------------- */
    void     (*set_pixel)(void* ctx, void* canvas, int x, int y, uint32_t color);
    uint32_t (*get_pixel)(void* ctx, void* canvas, int x, int y);

    /* ---- Blit (drawImage / patterns) ----------------------------------
     * `src` is an opaque bitmap handle (from bitmap_create, bitmap_from_element,
     * or another canvas). alpha is 0..255 (255 = opaque). smooth selects
     * bilinear/bicubic resampling when the source is scaled. */
    void (*blit)(void* ctx, void* canvas, void* src,
                 int sx, int sy, int sw, int sh,
                 int dx, int dy, int dw, int dh,
                 uint8_t alpha, int smooth);

    /* ---- Text ---------------------------------------------------------
     * `size` is the resolved font size in px; the embedder owns the font
     * handle (creating it lazily) and measures/draws with it. stroke_text
     * may be implemented as a series of offset draws or via a real outline
     * font; if NULL, the bridge approximates with draw_text. */
    void (*draw_text)(void* ctx, void* canvas, int x, int y, const char* text, float size, uint32_t color);
    void (*stroke_text)(void* ctx, void* canvas, int x, int y, const char* text, float size, int lw, uint32_t color);
    void (*text_size)(void* ctx, void* canvas, const char* text, float size, int* w, int* h);

    /* ---- Clipping -----------------------------------------------------
     * The graph library only supports rectangular clips (graph_set_clip),
     * so the bridge reduces the current path to its device-space bounding
     * box and hands that over. Path-accurate clipping would require a mask
     * bitmap; the bridge leaves that as a future embedder extension. */
    void (*set_clip)(void* ctx, void* canvas, int x, int y, int w, int h);
    void (*clear_clip)(void* ctx, void* canvas);
} js_canvas_callbacks_t;

/* Register the CanvasRenderingContext2D class (plus CanvasGradient,
 * CanvasPattern, TextMetrics, ImageData, Path2D, DOMMatrix) and hang
 * getContext()/width/height onto the "Element" class. `ctx` is passed
 * verbatim to every callback; `cb` is copied by value.
 *
 * Must be called AFTER vm_init() and AFTER js_register_dom_natives() (so the
 * Element class exists) and BEFORE vm_load()/vm_run().
 *
 * Returns true on success, false if vm/cb are NULL or registration failed. */
bool js_register_canvas_natives(vm_t* vm, void* ctx, const js_canvas_callbacks_t* cb);

#ifdef __cplusplus
}
#endif

#endif /* MARIO_JS_CANVAS_H */
