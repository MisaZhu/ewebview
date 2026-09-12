/*
 * js_canvas.c - HTML5 Canvas 2D natives for the mario JavaScript VM.
 *
 * See js_canvas.h for the contract. This file is pure C and depends only on
 * mario.h; every pixel is pushed through js_canvas_callbacks_t, so the bridge
 * knows nothing about graph_t/font_t/litehtml. Where the EwokOS graph library
 * already provides a primitive (rect, line, circle, arc, round-rect, blit,
 * pixel, clip), the bridge dispatches straight to the matching callback
 * instead of re-implementing it; the bridge only adds the geometry the graph
 * library does not have (polygon scanline fill, bezier/arc tessellation,
 * gradient span math, pattern tiling, dash segmentation, shadow offsets).
 *
 * Object model (mirrors js_dom.c):
 *   - A hidden "CanvasBridge" var hangs off vm->root under CANVAS_BRIDGE_KEY
 *     and carries the ctx pointer + callbacks + a registry of per-canvas
 *     context state in its ->value (a heap js_canvas_state). Every native
 *     receives it via the `data` argument, with the same state_from_vm()
 *     fallback js_dom uses for method-call paths that hand the native the env
 *     instead of the data.
 *   - getContext() creates (or reuses) an embedder canvas handle and wraps it
 *     in a js_canvas_ctx that owns the path/style/CTM/save/clip/dash state.
 *     That ctx is cached by canvas handle in the registry, so repeated
 *     getContext() calls on one <canvas> return contexts sharing state.
 *   - A CanvasRenderingContext2D instance's ->value is the js_canvas_ctx*;
 *     its free hook is a no-op because the registry (freed on vm_close) owns
 *     it, and the embedder owns the underlying bitmap.
 *   - CanvasGradient / CanvasPattern / ImageData / TextMetrics / DOMMatrix /
 *     Path2D instances likewise carry a heap struct in ->value with a free
 *     hook that releases it; the bridge registry keeps gradients/patterns
 *     alive while they are referenced as fillStyle/strokeStyle.
 *   - Path vertices are stored in DEVICE space (the CTM is applied as they
 *     are added, per spec), so fill/stroke need no transform at paint time.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "js_canvas.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#define CLS_ELEMENT   "Element"
#define CLS_CTX2D     "CanvasRenderingContext2D"
#define CLS_GRADIENT  "CanvasGradient"
#define CLS_PATTERN   "CanvasPattern"
#define CLS_IMAGEDATA "ImageData"
#define CLS_TEXTMETRICS "TextMetrics"
#define CLS_DOMMATRIX "DOMMatrix"
#define CLS_PATH2D    "Path2D"

/* Key under which the bridge state is stashed on vm->root (see js_dom.c). */
#define CANVAS_BRIDGE_KEY "@@canvas_bridge"

#define CV_PI 3.14159265358979323846f
#define CV_2PI (CV_PI * 2.0f)

/* Tessellation resolution: ~3 degrees per arc segment, capped so a full
 * circle never explodes the vertex buffer. */
#define CV_ARC_STEP (CV_PI / 60.0f)
#define CV_ARC_MAX  720
/* Bezier flattening tolerance in device pixels. */
#define CV_BEZ_TOL  0.5f
#define CV_BEZ_MAX  256

/* ------------------------------------------------------------------ */
/* Gradient / pattern / style                                         */
/* ------------------------------------------------------------------ */

typedef struct { float offset; uint32_t color; } cv_grad_stop_t;

typedef struct cv_gradient {
    int   type;                 /* JS_GRAD_LINEAR / RADIAL / CONIC */
    float x0, y0, x1, y1;       /* linear: start/end; radial: start/end centers */
    float r0, r1;               /* radial: start/end radii */
    float cx, cy;               /* conic: center */
    float angle;                /* conic: start angle */
    cv_grad_stop_t* stops;
    int   stops_n, stops_cap;
    int   refcount;             /* bumped while used as fill/strokeStyle */
} cv_gradient;

typedef struct cv_pattern {
    void* image;                /* opaque bitmap handle (embedder-owned) */
    int   repeat;               /* JS_PATTERN_* */
    /* Pattern transform (user space -> pattern space). Identity by default. */
    float a, b, c, d, e, f;
    int   refcount;
} cv_pattern;

/* A fill/stroke style is either a solid color or a gradient/pattern object.
 * The bridge keeps the object pointer alive via refcount while it is the
 * active style; on assignment the old object is released. */
typedef struct {
    int      kind;              /* 0 = color, 1 = gradient, 2 = pattern */
    uint32_t color;             /* kind 0 */
    void*    obj;               /* kind 1/2: cv_gradient* / cv_pattern* */
} cv_style;

/* ------------------------------------------------------------------ */
/* Per-canvas context state (pure C growable arrays)                  */
/* ------------------------------------------------------------------ */

typedef struct { float x; float y; } cv_point_t;

/* One subpath: a run of device-space points plus metadata for fast-path
 * dispatch (arc/rect/round-rect/circle) so fill/stroke can call the matching
 * graph primitive instead of the generic scanline/segment path. */
typedef struct {
    int   pts_start, pts_n;     /* slice into cv->pts */
    int   closed;
    int   kind;                 /* SUB_* below */
    float p[8];                 /* kind-specific params (user space) */
} cv_sub_t;

enum {
    SUB_GENERAL = 0,
    SUB_ARC,        /* p: cx, cy, r, a0, a1, ccw */
    SUB_RECT,       /* p: x, y, w, h */
    SUB_ROUND_RECT, /* p: x, y, w, h, r */
    SUB_CIRCLE,     /* p: cx, cy, r */
    SUB_ELLIPSE,    /* p: cx, cy, rx, ry, rot, a0, a1, ccw */
    SUB_QUADRATIC,  /* p: x0, y0, cx, cy, x1, y1 (single curve subpath) */
    SUB_CUBIC       /* p: x0, y0, cx1, cy1, cx2, cy2, x1, y1 */
};

/* Save-stack snapshot. Everything the spec says save() captures. */
typedef struct {
    cv_style fillStyle, strokeStyle;
    float    lineWidth, fontSize, miterLimit, lineDashOffset;
    float    globalAlpha;
    int      textAlign, textBaseline, lineCap, lineJoin;
    int      globalCompositeOperation;
    int      imageSmoothingEnabled, imageSmoothingQuality;
    int      direction;
    float    letterSpacing, wordSpacing;
    int      fontKerning, fontStretch, fontVariantCaps, textRendering;
    float    shadowBlur, shadowOffsetX, shadowOffsetY;
    uint32_t shadowColor;
    float    a, b, c, d, e, f;
    /* Clip is saved/restored as a device-space rect (0,0,0,0 = none). */
    int      clip_x, clip_y, clip_w, clip_h;
    int      has_clip;
    /* Dash pattern snapshot (variable length). */
    float*   dash;
    int      dash_n;
    /* Font family snapshot (heap string). */
    char*    fontFamily;
} cv_save_t;

typedef struct js_canvas_ctx {
    void*      canvas;          /* opaque embedder handle (owned by embedder) */
    int        canvas_w, canvas_h;

    /* Current path (device space). */
    cv_point_t* pts;    int pts_n,    pts_cap;
    cv_sub_t*   subs;   int subs_n,   subs_cap;

    /* Save stack. */
    cv_save_t*  saves;  int saves_n,  saves_cap;

    /* Scratch span buffer reused by the scanline fill. */
    float*      xs;     int xs_n,     xs_cap;

    /* Curve-flatten scratch: receives the polyline from the
     * flatten_quadratic/flatten_cubic callbacks (which wrap the graph
     * library's de Casteljau subdivision) so the bridge never re-implements
     * curve flattening. Interleaved x,y floats; cbuf_cap counts floats. */
    float*      cbuf;   int cbuf_cap;

    /* Style state. */
    cv_style fillStyle, strokeStyle;
    float    lineWidth, fontSize, miterLimit, lineDashOffset;
    float    globalAlpha;
    int      textAlign, textBaseline, lineCap, lineJoin;
    int      globalCompositeOperation;
    int      imageSmoothingEnabled, imageSmoothingQuality;
    int      direction;         /* 0=ltr, 1=rtl, 2=inherit */
    float    letterSpacing, wordSpacing;
    int      fontKerning, fontStretch, fontVariantCaps, textRendering;
    char*    fontFamily;        /* heap, from font shorthand */
    int      fontWeight;        /* 100..900, 400=normal, 700=bold */
    int      fontItalic;
    float    shadowBlur, shadowOffsetX, shadowOffsetY;
    uint32_t shadowColor;
    char*    filter;            /* accepted, currently ignored */

    /* Dash pattern (device-space lengths). Empty = solid. */
    float*   dash;      int dash_n, dash_cap;

    /* Clip rect in device space. has_clip=0 means no clip. */
    int      clip_x, clip_y, clip_w, clip_h;
    int      has_clip;

    /* CTM: x' = a*x + c*y + e ; y' = b*x + d*y + f */
    float a, b, c, d, e, f;

    /* Back-pointer to the bridge state (for callbacks during paint). */
    struct js_canvas_state* state;
} js_canvas_ctx;

typedef struct js_canvas_state {
    void*                 ctx;
    js_canvas_callbacks_t cb;
    js_canvas_ctx**       ctxs;     /* registry: one per live canvas handle */
    int                   ctxs_n, ctxs_cap;
    /* Live gradients/patterns. Kept here so a CanvasGradient instance that
     * JS dropped but is still referenced as fillStyle stays alive, and so
     * vm_close can release everything the bridge allocated. */
    cv_gradient**         grads;    int grads_n, grads_cap;
    cv_pattern**          pats;     int pats_n,  pats_cap;
    vm_t*                 vm;       /* for creating return vars in natives */
} js_canvas_state;

/* ---- small growable-array helpers (mario_malloc backed) ---- */

static void* cv_grow(void* p, int* cap, int need, size_t esz) {
    if(need <= *cap) return p;
    int nc = (*cap < 8) ? 8 : *cap;
    while(nc < need) nc *= 2;
    void* np = mario_malloc((uint32_t)(nc * (int)esz));
    if(np == NULL) return p;
    if(p != NULL) { memcpy(np, p, (size_t)(*cap) * esz); mario_free(p); }
    *cap = nc;
    return np;
}

static char* cv_strdup(const char* s) {
    if(s == NULL) return NULL;
    size_t n = strlen(s) + 1;
    char* p = (char*)mario_malloc((uint32_t)n);
    if(p != NULL) memcpy(p, s, n);
    return p;
}

/* ---- gradient / pattern lifecycle ---- */

static cv_gradient* cv_grad_new(int type) {
    cv_gradient* g = (cv_gradient*)mario_malloc(sizeof(cv_gradient));
    if(g == NULL) return NULL;
    memset(g, 0, sizeof(*g));
    g->type = type;
    g->refcount = 1;
    return g;
}

static void cv_grad_free(cv_gradient* g) {
    if(g == NULL) return;
    if(g->stops != NULL) mario_free(g->stops);
    mario_free(g);
}

static cv_pattern* cv_pat_new(void) {
    cv_pattern* p = (cv_pattern*)mario_malloc(sizeof(cv_pattern));
    if(p == NULL) return NULL;
    memset(p, 0, sizeof(*p));
    p->a = 1.0f; p->d = 1.0f;
    p->refcount = 1;
    return p;
}

static void cv_pat_free(cv_pattern* p) {
    if(p == NULL) return;
    mario_free(p);
}

/* Release a style's object reference. Called on reassignment and on ctx free. */
static void cv_style_release(js_canvas_state* st, cv_style* s);

static void cv_ctx_free(js_canvas_ctx* cv) {
    if(cv == NULL) return;
    if(cv->pts)    mario_free(cv->pts);
    if(cv->subs)   mario_free(cv->subs);
    if(cv->saves)  mario_free(cv->saves);
    if(cv->xs)     mario_free(cv->xs);
    if(cv->cbuf)   mario_free(cv->cbuf);
    if(cv->dash)   mario_free(cv->dash);
    if(cv->fontFamily) mario_free(cv->fontFamily);
    if(cv->filter) mario_free(cv->filter);
    cv_style_release(cv->state, &cv->fillStyle);
    cv_style_release(cv->state, &cv->strokeStyle);
    for(int i = 0; i < cv->saves_n; ++i) {
        if(cv->saves[i].dash != NULL) mario_free(cv->saves[i].dash);
        if(cv->saves[i].fontFamily != NULL) mario_free(cv->saves[i].fontFamily);
    }
    mario_free(cv);
}

static js_canvas_ctx* cv_ctx_new(void* canvas, int w, int h, js_canvas_state* st) {
    js_canvas_ctx* cv = (js_canvas_ctx*)mario_malloc(sizeof(js_canvas_ctx));
    if(cv == NULL) return NULL;
    memset(cv, 0, sizeof(*cv));
    cv->canvas      = canvas;
    cv->canvas_w    = w;
    cv->canvas_h    = h;
    cv->state       = st;
    cv->fillStyle.kind  = 0; cv->fillStyle.color  = 0xFF000000u;
    cv->strokeStyle.kind= 0; cv->strokeStyle.color= 0xFF000000u;
    cv->lineWidth   = 1.0f;
    cv->fontSize    = 10.0f;
    cv->miterLimit  = 10.0f;
    cv->globalAlpha = 1.0f;
    cv->textAlign   = JS_TEXT_ALIGN_START;
    cv->textBaseline= JS_TEXT_BASELINE_ALPHABETIC;
    cv->lineCap     = JS_LINE_CAP_BUTT;
    cv->lineJoin    = JS_LINE_JOIN_MITER;
    cv->globalCompositeOperation = JS_GCO_SOURCE_OVER;
    cv->imageSmoothingEnabled = 1;
    cv->imageSmoothingQuality = JS_SMOOTH_LOW;
    cv->direction   = 2;        /* inherit */
    cv->fontKerning = 0;        /* auto */
    cv->fontStretch = 0;        /* normal */
    cv->fontVariantCaps = 0;    /* normal */
    cv->textRendering = 0;      /* auto */
    cv->fontWeight  = 400;
    cv->fontItalic  = 0;
    cv->shadowColor = 0x00000000u;  /* fully transparent black */
    cv->fontFamily  = cv_strdup("sans-serif");
    cv->a = 1.0f; cv->b = 0.0f; cv->c = 0.0f; cv->d = 1.0f; cv->e = 0.0f; cv->f = 0.0f;
    return cv;
}

/* ------------------------------------------------------------------ */
/* Bridge state plumbing                                              */
/* ------------------------------------------------------------------ */

static void canvas_state_free(void* p) {
    js_canvas_state* st = (js_canvas_state*)p;
    if(st == NULL) return;
    for(int i = 0; i < st->ctxs_n; ++i) cv_ctx_free(st->ctxs[i]);
    if(st->ctxs != NULL) mario_free(st->ctxs);
    for(int i = 0; i < st->grads_n; ++i) cv_grad_free(st->grads[i]);
    if(st->grads != NULL) mario_free(st->grads);
    for(int i = 0; i < st->pats_n; ++i) cv_pat_free(st->pats[i]);
    if(st->pats != NULL) mario_free(st->pats);
    mario_free(st);
}

static js_canvas_state* state_from_data(void* data) {
    if(data == NULL) return NULL;
    var_t* bridge = (var_t*)data;
    return (js_canvas_state*)bridge->value;
}

static js_canvas_state* state_from_vm(vm_t* vm) {
    if(vm == NULL || vm->root == NULL) return NULL;
    var_t* bridge = var_find_own_member_var(vm->root, CANVAS_BRIDGE_KEY);
    if(bridge == NULL) return NULL;
    return (js_canvas_state*)bridge->value;
}

static js_canvas_state* state_any(vm_t* vm, void* data) {
    js_canvas_state* st = state_from_data(data);
    if(st != NULL) return st;
    return state_from_vm(vm);
}

/* Find or create the context bound to an embedder canvas handle so repeated
 * getContext() calls share path/style state (see the object-model note). */
static js_canvas_ctx* ctx_for_canvas(js_canvas_state* st, void* canvas, int w, int h) {
    if(st == NULL || canvas == NULL) return NULL;
    for(int i = 0; i < st->ctxs_n; ++i)
        if(st->ctxs[i] != NULL && st->ctxs[i]->canvas == canvas)
            return st->ctxs[i];
    js_canvas_ctx* cv = cv_ctx_new(canvas, w, h, st);
    if(cv == NULL) return NULL;
    st->ctxs = (js_canvas_ctx**)cv_grow(st->ctxs, &st->ctxs_cap, st->ctxs_n + 1, sizeof(js_canvas_ctx*));
    if(st->ctxs_n + 1 > st->ctxs_cap) { cv_ctx_free(cv); return NULL; }
    st->ctxs[st->ctxs_n++] = cv;
    return cv;
}

/* Recover the js_canvas_ctx a ctx-method native was invoked on (this->value). */
static js_canvas_ctx* ctx_from_env(var_t* env) {
    var_t* t = get_obj(env, THIS);
    if(t == NULL) return NULL;
    return (js_canvas_ctx*)t->value;
}

/* Register a getter/setter accessor property (identical layout to js_dom.c's
 * reg_accessor; the canvas ctx natives recover their state from `this`, so no
 * native data pointer is needed and NULL is registered). */
static void reg_accessor(vm_t* vm, var_t* cls, const char* prop,
                         native_func_t getter, native_func_t setter) {
    char decl[96];
    snprintf(decl, sizeof(decl), "%s()", prop);
    node_t* gn = vm_reg_native(vm, cls, decl, getter, NULL);
    if(gn == NULL || gn->var == NULL) return;
    func_t* gf = var_get_func(gn->var);
    if(gf != NULL) gf->regular = FUNC_GETTER;
    if(setter == NULL) return;
    node_t* sn = vm_reg_native_on(vm, gn->var, FUNC_SETTER_KEY "(v)", setter, NULL);
    if(sn == NULL || sn->var == NULL) return;
    func_t* sf = var_get_func(sn->var);
    if(sf != NULL) sf->regular = FUNC_SETTER;
    sn->invisable = 1;
    sn->be_unenumerable = 1;
}

/* Read the single value an accessor setter was assigned (mario passes it as
 * args[0]; see handle_asign -> func_call(vm, obj, setter, 1)). */
static node_t* setter_arg(var_t* env) {
    var_t* args = get_func_args(env);
    return var_array_get(args, 0);
}

/* ------------------------------------------------------------------ */
/* CSS value parsing                                                  */
/* ------------------------------------------------------------------ */

static int hexval(char ch) {
    if(ch >= '0' && ch <= '9') return ch - '0';
    if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return 0;
}

static uint32_t clamp_u8(int v) {
    if(v < 0) return 0;
    if(v > 255) return 255;
    return (uint32_t)v;
}

/* Parse a CSS color to 0xAARRGGBB. Supports #rgb, #rgba, #rrggbb, #rrggbbaa,
 * rgb()/rgba(), hsl()/hsla(), and the 147 CSS named colors. Unknown -> opaque
 * black (per spec, an unparseable value leaves the previous style unchanged,
 * but the bridge keeps it simple and falls back to black). */
static uint32_t parse_color(const char* s);

/* Named-color table (CSS Color Module Level 4). Sorted alphabetically for
 * bsearch; 147 entries. */
typedef struct { const char* name; uint32_t argb; } cv_named_color_t;
static const cv_named_color_t CV_NAMED_COLORS[] = {
    {"aliceblue",0xFFF0F8FFu},{"antiquewhite",0xFFFAEBD7u},{"aqua",0xFF00FFFFu},
    {"aquamarine",0xFF7FFFD4u},{"azure",0xFFF0FFFFu},{"beige",0xFFF5F5DCu},
    {"bisque",0xFFFFE4C4u},{"black",0xFF000000u},{"blanchedalmond",0xFFFFEBCDu},
    {"blue",0xFF0000FFu},{"blueviolet",0xFF8A2BE2u},{"brown",0xFFA52A2Au},
    {"burlywood",0xFFDEB887u},{"cadetblue",0xFF5F9EA0u},{"chartreuse",0xFF7FFF00u},
    {"chocolate",0xFFD2691Eu},{"coral",0xFFFF7F50u},{"cornflowerblue",0xFF6495EDu},
    {"cornsilk",0xFFFFF8DCu},{"crimson",0xFFDC143Cu},{"cyan",0xFF00FFFFu},
    {"darkblue",0xFF00008Bu},{"darkcyan",0xFF008B8Bu},{"darkgoldenrod",0xFFB8860Bu},
    {"darkgray",0xFFA9A9A9u},{"darkgreen",0xFF006400u},{"darkgrey",0xFFA9A9A9u},
    {"darkkhaki",0xFFBDB76Bu},{"darkmagenta",0xFF8B008Bu},{"darkolivegreen",0xFF556B2Fu},
    {"darkorange",0xFFFF8C00u},{"darkorchid",0xFF9932CCu},{"darkred",0xFF8B0000u},
    {"darksalmon",0xFFE9967Au},{"darkseagreen",0xFF8FBC8Fu},{"darkslateblue",0xFF483D8Bu},
    {"darkslategray",0xFF2F4F4Fu},{"darkslategrey",0xFF2F4F4Fu},{"darkturquoise",0xFF00CED1u},
    {"darkviolet",0xFF9400D3u},{"deeppink",0xFFFF1493u},{"deepskyblue",0xFF00BFFFu},
    {"dimgray",0xFF696969u},{"dimgrey",0xFF696969u},{"dodgerblue",0xFF1E90FFu},
    {"firebrick",0xFFB22222u},{"floralwhite",0xFFFFFAF0u},{"forestgreen",0xFF228B22u},
    {"fuchsia",0xFFFF00FFu},{"gainsboro",0xFFDCDCDCu},{"ghostwhite",0xFFF8F8FFu},
    {"gold",0xFFFFD700u},{"goldenrod",0xFFDAA520u},{"gray",0xFF808080u},
    {"green",0xFF008000u},{"greenyellow",0xFFADFF2Fu},{"grey",0xFF808080u},
    {"honeydew",0xFFF0FFF0u},{"hotpink",0xFFFF69B4u},{"indianred",0xFFCD5C5Cu},
    {"indigo",0xFF4B0082u},{"ivory",0xFFFFFFF0u},{"khaki",0xFFF0E68Cu},
    {"lavender",0xFFE6E6FAu},{"lavenderblush",0xFFFFF0F5u},{"lawngreen",0xFF7CFC00u},
    {"lemonchiffon",0xFFFFFACDu},{"lightblue",0xFFADD8E6u},{"lightcoral",0xFFF08080u},
    {"lightcyan",0xFFE0FFFFu},{"lightgoldenrodyellow",0xFFFAFAD2u},{"lightgray",0xFFD3D3D3u},
    {"lightgreen",0xFF90EE90u},{"lightgrey",0xFFD3D3D3u},{"lightpink",0xFFFFB6C1u},
    {"lightsalmon",0xFFFFA07Au},{"lightseagreen",0xFF20B2AAu},{"lightskyblue",0xFF87CEFAu},
    {"lightslategray",0xFF778899u},{"lightslategrey",0xFF778899u},{"lightsteelblue",0xFFB0C4DEu},
    {"lightyellow",0xFFFFE082u},{"lime",0xFF00FF00u},{"limegreen",0xFF32CD32u},
    {"linen",0xFFFAF0E6u},{"magenta",0xFFFF00FFu},{"maroon",0xFF800000u},
    {"mediumaquamarine",0xFF66CDAAu},{"mediumblue",0xFF0000CDu},{"mediumorchid",0xFFBA55D3u},
    {"mediumpurple",0xFF9370DBu},{"mediumseagreen",0xFF3CB371u},{"mediumslateblue",0xFF7B68EEu},
    {"mediumspringgreen",0xFF00FA9Au},{"mediumturquoise",0xFF48D1CCu},{"mediumvioletred",0xFFC71585u},
    {"midnightblue",0xFF191970u},{"mintcream",0xFFF5FFFAu},{"mistyrose",0xFFFFE4E1u},
    {"moccasin",0xFFFFE4B5u},{"navajowhite",0xFFFFDEADu},{"navy",0xFF000080u},
    {"oldlace",0xFFFDF5E6u},{"olive",0xFF808000u},{"olivedrab",0xFF6B8E23u},
    {"orange",0xFFFFA500u},{"orangered",0xFFFF4500u},{"orchid",0xFFDA70D6u},
    {"palegoldenrod",0xFFEEE8AAu},{"palegreen",0xFF98FB98u},{"paleturquoise",0xFFAFEEEEu},
    {"palevioletred",0xFFDB7093u},{"papayawhip",0xFFFFEFD5u},{"peachpuff",0xFFFFDAB9u},
    {"peru",0xFFCD853Fu},{"pink",0xFFFFC0CBu},{"plum",0xFFDDA0DDu},
    {"powderblue",0xFFB0E0E6u},{"purple",0xFF800080u},{"rebeccapurple",0xFF663399u},
    {"red",0xFFFF0000u},{"rosybrown",0xFFBC8F8Fu},{"royalblue",0xFF4169E1u},
    {"saddlebrown",0xFF8B4513u},{"salmon",0xFFFA8072u},{"sandybrown",0xFFF4A460u},
    {"seagreen",0xFF2E8B57u},{"seashell",0xFFFFF5EEu},{"sienna",0xFFA0522Du},
    {"silver",0xFFC0C0C0u},{"skyblue",0xFF87CEEBu},{"slateblue",0xFF6A5ACDu},
    {"slategray",0xFF708090u},{"slategrey",0xFF708090u},{"snow",0xFFFFFAFAu},
    {"springgreen",0xFF00FF7Fu},{"steelblue",0xFF4682B4u},{"tan",0xFFD2B48Cu},
    {"teal",0xFF008080u},{"thistle",0xFFD8BFD8u},{"tomato",0xFFFF6347u},
    {"transparent",0x00000000u},{"turquoise",0xFF40E0D0u},{"violet",0xFFEE82EEu},
    {"wheat",0xFFF5DEB3u},{"white",0xFFFFFFFFu},{"whitesmoke",0xFFF5F5F5u},
    {"yellow",0xFFFFFF00u},{"yellowgreen",0xFF9ACD32u}
};
#define CV_NAMED_COLORS_N ((int)(sizeof(CV_NAMED_COLORS)/sizeof(CV_NAMED_COLORS[0])))

static int cv_named_cmp(const void* a, const void* b) {
    const cv_named_color_t* x = (const cv_named_color_t*)a;
    const cv_named_color_t* y = (const cv_named_color_t*)b;
    return strcasecmp(x->name, y->name);
}

/* Parse "rgb(r,g,b)" / "rgba(r,g,b,a)" with 0..255 ints or percentages, and
 * the modern space-separated "rgb(r g b / a)" form. */
static int parse_rgb_func(const char* s, uint32_t* out) {
    /* Skip "rgb(" or "rgba(" */
    const char* p = s + 4;
    if(*p == 'a') p++;
    if(*p != '(') return 0;
    p++;
    float v[4] = {0,0,0,1};
    int   pct[4] = {0,0,0,0};
    int   i = 0;
    while(*p && i < 4) {
        while(*p == ' ' || *p == ',' || *p == '/') p++;
        if(*p == ')') break;
        if(!(*p == '-' || (*p >= '0' && *p <= '9') || *p == '.')) break;
        float sign = 1.0f;
        if(*p == '-') { sign = -1.0f; p++; }
        float num = 0.0f;
        while(*p >= '0' && *p <= '9') { num = num * 10.0f + (float)(*p - '0'); p++; }
        if(*p == '.') {
            p++;
            float sc = 0.1f;
            while(*p >= '0' && *p <= '9') { num += (float)(*p - '0') * sc; sc *= 0.1f; p++; }
        }
        num *= sign;
        if(*p == '%') { pct[i] = 1; p++; }
        v[i++] = num;
    }
    if(i < 3) return 0;
    int r = pct[0] ? (int)(v[0] * 2.55f + 0.5f) : (int)(v[0] + 0.5f);
    int g = pct[1] ? (int)(v[1] * 2.55f + 0.5f) : (int)(v[1] + 0.5f);
    int b = pct[2] ? (int)(v[2] * 2.55f + 0.5f) : (int)(v[2] + 0.5f);
    int a = 255;
    if(i >= 4) a = pct[3] ? (int)(v[3] * 2.55f + 0.5f) : (int)(v[3] * 255.0f + 0.5f);
    *out = (clamp_u8(a) << 24) | (clamp_u8(r) << 16) | (clamp_u8(g) << 8) | clamp_u8(b);
    return 1;
}

/* HSL -> RGB. h in [0,360), s/l in [0,1]. */
static void hsl_to_rgb(float h, float s, float l, int* r, int* g, int* b) {
    h = fmodf(h, 360.0f); if(h < 0) h += 360.0f;
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r1=0,g1=0,b1=0;
    if(hp < 1)      { r1=c; g1=x; }
    else if(hp < 2) { r1=x; g1=c; }
    else if(hp < 3) { g1=c; b1=x; }
    else if(hp < 4) { g1=x; b1=c; }
    else if(hp < 5) { r1=x; b1=c; }
    else            { r1=c; b1=x; }
    float m = l - c * 0.5f;
    *r = (int)((r1 + m) * 255.0f + 0.5f);
    *g = (int)((g1 + m) * 255.0f + 0.5f);
    *b = (int)((b1 + m) * 255.0f + 0.5f);
}

static int parse_hsl_func(const char* s, uint32_t* out) {
    const char* p = s + 4;
    if(*p == 'a') p++;
    if(*p != '(') return 0;
    p++;
    float v[4] = {0,0,0,1};
    int   pct[4] = {0,0,0,0};
    int   i = 0;
    while(*p && i < 4) {
        while(*p == ' ' || *p == ',' || *p == '/') p++;
        if(*p == ')') break;
        if(!(*p == '-' || (*p >= '0' && *p <= '9') || *p == '.')) break;
        float sign = 1.0f;
        if(*p == '-') { sign = -1.0f; p++; }
        float num = 0.0f;
        while(*p >= '0' && *p <= '9') { num = num * 10.0f + (float)(*p - '0'); p++; }
        if(*p == '.') {
            p++;
            float sc = 0.1f;
            while(*p >= '0' && *p <= '9') { num += (float)(*p - '0') * sc; sc *= 0.1f; p++; }
        }
        num *= sign;
        if(*p == '%') { pct[i] = 1; p++; }
        if(i == 0 && (*p == 'd' || *p == 'r' || *p == 'g' || *p == 't')) {
            /* deg/rad/grad/turn suffix */
            if(!strncmp(p, "deg", 3)) { p += 3; }
            else if(!strncmp(p, "rad", 3)) { num *= 180.0f / CV_PI; p += 3; }
            else if(!strncmp(p, "grad", 4)) { num *= 0.9f; p += 4; }
            else if(!strncmp(p, "turn", 4)) { num *= 360.0f; p += 4; }
        }
        v[i++] = num;
    }
    if(i < 3) return 0;
    float h = v[0];
    float sv = pct[1] ? v[1] / 100.0f : v[1];
    float lv = pct[2] ? v[2] / 100.0f : v[2];
    int r,g,b; hsl_to_rgb(h, sv, lv, &r, &g, &b);
    int a = 255;
    if(i >= 4) a = pct[3] ? (int)(v[3] * 2.55f + 0.5f) : (int)(v[3] * 255.0f + 0.5f);
    *out = (clamp_u8(a) << 24) | (clamp_u8(r) << 16) | (clamp_u8(g) << 8) | clamp_u8(b);
    return 1;
}

static uint32_t parse_color(const char* s) {
    if(s == NULL) return 0xFF000000u;
    while(*s == ' ' || *s == '\t') s++;
    if(s[0] == '#') {
        size_t n = strlen(s + 1);
        if(n == 3) {
            int r = hexval(s[1]), g = hexval(s[2]), b = hexval(s[3]);
            return 0xFF000000u | ((uint32_t)(r*17)<<16) | ((uint32_t)(g*17)<<8) | (uint32_t)(b*17);
        }
        if(n == 4) {
            int r = hexval(s[1]), g = hexval(s[2]), b = hexval(s[3]), a = hexval(s[4]);
            return ((uint32_t)(a*17)<<24) | ((uint32_t)(r*17)<<16) | ((uint32_t)(g*17)<<8) | (uint32_t)(b*17);
        }
        if(n == 6) {
            int r = (hexval(s[1])<<4)|hexval(s[2]);
            int g = (hexval(s[3])<<4)|hexval(s[4]);
            int b = (hexval(s[5])<<4)|hexval(s[6]);
            return 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
        }
        if(n >= 8) {
            int r = (hexval(s[1])<<4)|hexval(s[2]);
            int g = (hexval(s[3])<<4)|hexval(s[4]);
            int b = (hexval(s[5])<<4)|hexval(s[6]);
            int a = (hexval(s[7])<<4)|hexval(s[8]);
            return ((uint32_t)a<<24) | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
        }
        return 0xFF000000u;
    }
    if(!strncasecmp(s, "rgb", 3)) {
        uint32_t out;
        if(parse_rgb_func(s, &out)) return out;
        return 0xFF000000u;
    }
    if(!strncasecmp(s, "hsl", 3)) {
        uint32_t out;
        if(parse_hsl_func(s, &out)) return out;
        return 0xFF000000u;
    }
    cv_named_color_t key; key.name = s; key.argb = 0;
    cv_named_color_t* hit = (cv_named_color_t*)bsearch(&key, CV_NAMED_COLORS,
            (size_t)CV_NAMED_COLORS_N, sizeof(cv_named_color_t), cv_named_cmp);
    if(hit != NULL) return hit->argb;
    return 0xFF000000u;
}

/* Multiply a color's alpha channel by `a` (0..1). Used to fold globalAlpha
 * into every paint call so the embedder does not need a separate alpha arg. */
static uint32_t color_scale_alpha(uint32_t c, float a) {
    if(a >= 1.0f) return c;
    if(a <= 0.0f) return c & 0x00FFFFFFu;
    int ca = (int)((c >> 24) & 0xFF);
    int na = (int)(ca * a + 0.5f);
    return ((uint32_t)clamp_u8(na) << 24) | (c & 0x00FFFFFFu);
}

/* Linear interpolation between two ARGB colors, t in [0,1]. */
static uint32_t color_lerp(uint32_t c0, uint32_t c1, float t) {
    if(t <= 0.0f) return c0;
    if(t >= 1.0f) return c1;
    int a0 = (int)((c0>>24)&0xFF), r0 = (int)((c0>>16)&0xFF), g0 = (int)((c0>>8)&0xFF), b0 = (int)(c0&0xFF);
    int a1 = (int)((c1>>24)&0xFF), r1 = (int)((c1>>16)&0xFF), g1 = (int)((c1>>8)&0xFF), b1 = (int)(c1&0xFF);
    int a = (int)(a0 + (a1-a0)*t + 0.5f);
    int r = (int)(r0 + (r1-r0)*t + 0.5f);
    int g = (int)(g0 + (g1-g0)*t + 0.5f);
    int b = (int)(b0 + (b1-b0)*t + 0.5f);
    return ((uint32_t)clamp_u8(a)<<24) | ((uint32_t)clamp_u8(r)<<16) | ((uint32_t)clamp_u8(g)<<8) | (uint32_t)clamp_u8(b);
}

/* Pull the leading px size out of a CSS font shorthand ("24px Arial" -> 24).
 * Parsed by hand: this toolchain's libc declares strtol but not strtof. */
static float parse_font_size(const char* s) {
    if(s == NULL) return 10.0f;
    const char* p = s;
    while(*p && !((*p >= '0' && *p <= '9') || *p == '.')) p++;
    float v = 0.0f;
    while(*p >= '0' && *p <= '9') { v = v * 10.0f + (float)(*p - '0'); p++; }
    if(*p == '.') {
        p++;
        float scale = 0.1f;
        while(*p >= '0' && *p <= '9') { v += (float)(*p - '0') * scale; scale *= 0.1f; p++; }
    }
    return (v > 0.0f) ? v : 10.0f;
}

/* Parse a CSS font shorthand into size + family + weight + style. The bridge
 * only needs size for measurement and family for the embedder's font lookup;
 * weight/style are recorded so get_font can round-trip them. */
static void parse_font_shorthand(const char* s, float* size, char** family,
                                 int* weight, int* italic) {
    if(s == NULL) return;
    *size = parse_font_size(s);
    /* Find the family: everything after the size token's unit. */
    const char* p = s;
    while(*p && !((*p >= '0' && *p <= '9') || *p == '.')) p++;
    while(*p && ((*p >= '0' && *p <= '9') || *p == '.')) p++;
    /* Skip unit (px, pt, em, rem, %). */
    while(*p && *p != ' ' && *p != '\t') p++;
    while(*p == ' ' || *p == '\t') p++;
    /* Skip style/variant/weight tokens before the family. */
    for(;;) {
        if(!strncasecmp(p, "italic", 6))      { *italic = 1; p += 6; }
        else if(!strncasecmp(p, "oblique", 7)){ *italic = 1; p += 7; }
        else if(!strncasecmp(p, "normal", 6)) { p += 6; }
        else if(!strncasecmp(p, "bold", 4))   { *weight = 700; p += 4; }
        else if(!strncasecmp(p, "bolder", 6)) { *weight = 700; p += 6; }
        else if(!strncasecmp(p, "lighter", 7)){ *weight = 300; p += 7; }
        else if(*p >= '0' && *p <= '9') {
            int w = 0;
            while(*p >= '0' && *p <= '9') { w = w*10 + (*p - '0'); p++; }
            if(w >= 100 && w <= 900) *weight = w;
        }
        else break;
        while(*p == ' ' || *p == '\t') p++;
    }
    if(*family != NULL) { mario_free(*family); *family = NULL; }
    if(*p != 0) *family = cv_strdup(p);
}

/* Parse a single float from the head of a string (for setLineDash etc.). */
static float cv_atof(const char* s) {
    if(s == NULL) return 0.0f;
    while(*s == ' ' || *s == '\t') s++;
    float sign = 1.0f;
    if(*s == '-') { sign = -1.0f; s++; }
    else if(*s == '+') s++;
    float v = 0.0f;
    while(*s >= '0' && *s <= '9') { v = v * 10.0f + (float)(*s - '0'); s++; }
    if(*s == '.') {
        s++;
        float sc = 0.1f;
        while(*s >= '0' && *s <= '9') { v += (float)(*s - '0') * sc; sc *= 0.1f; s++; }
    }
    if(*s == 'e' || *s == 'E') {
        s++;
        int esign = 1;
        if(*s == '-') { esign = -1; s++; }
        else if(*s == '+') s++;
        int e = 0;
        while(*s >= '0' && *s <= '9') { e = e*10 + (*s - '0'); s++; }
        while(e-- > 0) v *= (esign > 0) ? 10.0f : 0.1f;
    }
    return v * sign;
}


/* ------------------------------------------------------------------ */
/* Path geometry (device space)                                       */
/* ------------------------------------------------------------------ */

static inline void cv_xform(const js_canvas_ctx* cv, float x, float y, float* ox, float* oy) {
    *ox = cv->a * x + cv->c * y + cv->e;
    *oy = cv->b * x + cv->d * y + cv->f;
}

/* Determinant of the CTM's linear part; used to scale line widths and
 * dash lengths from user space into device space. */
static inline float cv_det(const js_canvas_ctx* cv) {
    return cv->a * cv->d - cv->b * cv->c;
}

/* Average scale factor of the CTM (sqrt(|det|)), used to scale lineWidth. */
static inline float cv_scale(const js_canvas_ctx* cv) {
    float dt = cv_det(cv);
    if(dt < 0) dt = -dt;
    return sqrtf(dt);
}

static void cv_begin_sub_kind(js_canvas_ctx* cv, float ux, float uy, int kind) {
    cv->subs = (cv_sub_t*)cv_grow(cv->subs, &cv->subs_cap, cv->subs_n + 1, sizeof(cv_sub_t));
    if(cv->subs_n + 1 > cv->subs_cap) return;
    cv_sub_t* s = &cv->subs[cv->subs_n++];
    memset(s, 0, sizeof(*s));
    s->pts_start = cv->pts_n;
    s->pts_n = 0;
    s->kind = kind;
    cv_point_t p; cv_xform(cv, ux, uy, &p.x, &p.y);
    cv->pts = (cv_point_t*)cv_grow(cv->pts, &cv->pts_cap, cv->pts_n + 1, sizeof(cv_point_t));
    if(cv->pts_n + 1 > cv->pts_cap) return;
    cv->pts[cv->pts_n++] = p;
    s->pts_n = 1;
}

static void cv_begin_sub(js_canvas_ctx* cv, float ux, float uy) {
    cv_begin_sub_kind(cv, ux, uy, SUB_GENERAL);
}

static void cv_add_pt(js_canvas_ctx* cv, float ux, float uy) {
    if(cv->subs_n == 0) { cv_begin_sub(cv, ux, uy); return; }
    cv_sub_t* s = &cv->subs[cv->subs_n - 1];
    cv_point_t p; cv_xform(cv, ux, uy, &p.x, &p.y);
    cv->pts = (cv_point_t*)cv_grow(cv->pts, &cv->pts_cap, cv->pts_n + 1, sizeof(cv_point_t));
    if(cv->pts_n + 1 > cv->pts_cap) return;
    cv->pts[cv->pts_n++] = p;
    s->pts_n++;
    /* Adding a generic point downgrades a typed subpath to general. */
    if(s->kind != SUB_GENERAL) s->kind = SUB_GENERAL;
}

/* Add a raw device-space point (used by tessellation helpers that already
 * applied the CTM). */
static void cv_add_pt_dev(js_canvas_ctx* cv, float dx, float dy) {
    if(cv->subs_n == 0) {
        cv->subs = (cv_sub_t*)cv_grow(cv->subs, &cv->subs_cap, 1, sizeof(cv_sub_t));
        if(cv->subs_cap < 1) return;
        cv_sub_t* s = &cv->subs[cv->subs_n++];
        memset(s, 0, sizeof(*s));
        s->pts_start = cv->pts_n;
        s->kind = SUB_GENERAL;
    }
    cv_sub_t* s = &cv->subs[cv->subs_n - 1];
    cv->pts = (cv_point_t*)cv_grow(cv->pts, &cv->pts_cap, cv->pts_n + 1, sizeof(cv_point_t));
    if(cv->pts_n + 1 > cv->pts_cap) return;
    cv->pts[cv->pts_n].x = dx;
    cv->pts[cv->pts_n].y = dy;
    cv->pts_n++;
    s->pts_n++;
    if(s->kind != SUB_GENERAL) s->kind = SUB_GENERAL;
}

static void cv_close_sub(js_canvas_ctx* cv) {
    if(cv->subs_n == 0) return;
    cv->subs[cv->subs_n - 1].closed = 1;
}

/* Record kind-specific params on the current subpath (user space). */
static void cv_set_sub_params(js_canvas_ctx* cv, const float* p, int n) {
    if(cv->subs_n == 0) return;
    cv_sub_t* s = &cv->subs[cv->subs_n - 1];
    for(int i = 0; i < n && i < 8; ++i) s->p[i] = p[i];
}

/* ---- Curve flattening (delegated to the graph library) ---- */

/* Flatten a quadratic/cubic bezier by delegating to the embedder's
 * flatten_quadratic/flatten_cubic callbacks, which wrap the graph library's
 * adaptive de Casteljau subdivision (graph/curve.h). The bridge must NOT
 * re-implement that math. The returned polyline is in the SAME space as the
 * control points we pass (user space), so each vertex is baked through
 * cv_add_pt (which applies the CTM). p0 is the current pen position.
 *
 * If the callback is unavailable (minimal embedder) we degrade to a straight
 * line to the end point so the path still closes. */
static void cv_flatten_quadratic(js_canvas_ctx* cv, float p0x, float p0y,
                                 float cpx, float cpy, float x, float y) {
    js_canvas_state* st = cv->state;
    if(st == NULL || st->cb.flatten_quadratic == NULL) { cv_add_pt(cv, x, y); return; }
    const int max = CV_BEZ_MAX;
    cv->cbuf = (float*)cv_grow(cv->cbuf, &cv->cbuf_cap, max * 2, sizeof(float));
    if(cv->cbuf_cap < max * 2) { cv_add_pt(cv, x, y); return; }
    int n = st->cb.flatten_quadratic(st->ctx, p0x, p0y, cpx, cpy, x, y, cv->cbuf, max);
    if(n <= 0) { cv_add_pt(cv, x, y); return; }
    for(int i = 0; i < n; i++)
        cv_add_pt(cv, cv->cbuf[2 * i], cv->cbuf[2 * i + 1]);
}

static void cv_flatten_cubic(js_canvas_ctx* cv, float p0x, float p0y,
                             float c1x, float c1y, float c2x, float c2y,
                             float x, float y) {
    js_canvas_state* st = cv->state;
    if(st == NULL || st->cb.flatten_cubic == NULL) { cv_add_pt(cv, x, y); return; }
    const int max = CV_BEZ_MAX;
    cv->cbuf = (float*)cv_grow(cv->cbuf, &cv->cbuf_cap, max * 2, sizeof(float));
    if(cv->cbuf_cap < max * 2) { cv_add_pt(cv, x, y); return; }
    int n = st->cb.flatten_cubic(st->ctx, p0x, p0y, c1x, c1y, c2x, c2y, x, y, cv->cbuf, max);
    if(n <= 0) { cv_add_pt(cv, x, y); return; }
    for(int i = 0; i < n; i++)
        cv_add_pt(cv, cv->cbuf[2 * i], cv->cbuf[2 * i + 1]);
}

/* Tessellate an elliptical arc (user space) into the current subpath.
 * cx,cy center; rx,ry radii; rot x-axis rotation (radians); a0,a1 start/end
 * angles; ccw counter-clockwise flag. If move_first, begin a new subpath. */
static void cv_tess_ellipse(js_canvas_ctx* cv, float cx, float cy,
                            float rx, float ry, float rot,
                            float a0, float a1, int ccw, int move_first) {
    if(rx < 0) rx = -rx;
    if(ry < 0) ry = -ry;
    if(rx == 0 || ry == 0) return;
    /* Normalize sweep per spec: if !ccw and a1-a0 <= -2pi, clamp; if ccw and
     * a0-a1 <= -2pi, clamp. Otherwise adjust a1 by +/- 2pi to get the right
     * direction. */
    float sweep;
    if(ccw) {
        sweep = a0 - a1;
        while(sweep < 0) sweep += CV_2PI;
        if(sweep > CV_2PI) sweep = CV_2PI;
        sweep = -sweep;
    } else {
        sweep = a1 - a0;
        while(sweep < 0) sweep += CV_2PI;
        if(sweep > CV_2PI) sweep = CV_2PI;
    }
    int n = (int)(fabsf(sweep) / CV_ARC_STEP) + 2;
    if(n < 2) n = 2;
    if(n > CV_ARC_MAX) n = CV_ARC_MAX;
    float co = cosf(rot), si = sinf(rot);
    for(int i = 0; i <= n; i++) {
        float t = a0 + sweep * ((float)i / (float)n);
        float px = rx * cosf(t);
        float py = ry * sinf(t);
        float ux = cx + px * co - py * si;
        float uy = cy + px * si + py * co;
        if(i == 0 && move_first) cv_begin_sub(cv, ux, uy);
        else cv_add_pt(cv, ux, uy);
    }
}

/* ------------------------------------------------------------------ */
/* Scanline polygon fill (device space)                               */
/*                                                                    */
/* The graph library has no general polygon primitive, so the bridge  */
/* does an even-odd scanline fill and dispatches each span through    */
/* fill_rect. For gradient/pattern styles the span color is computed  */
/* per-pixel (gradient) or per-tile (pattern) instead of being solid. */
/* ------------------------------------------------------------------ */

/* Compute the gradient color at device-space (x,y). */
static uint32_t cv_gradient_color(const cv_gradient* g, float x, float y) {
    if(g == NULL || g->stops_n == 0) return 0xFF000000u;
    float t;
    if(g->type == JS_GRAD_LINEAR) {
        float dx = g->x1 - g->x0, dy = g->y1 - g->y0;
        float len2 = dx*dx + dy*dy;
        if(len2 <= 0.0f) t = 0.0f;
        else t = ((x - g->x0) * dx + (y - g->y0) * dy) / len2;
    } else if(g->type == JS_GRAD_RADIAL) {
        /* Distance from the start circle, interpolated to the end circle.
         * Simplified: project onto the line between centers, scale by radii. */
        float dx = g->x1 - g->x0, dy = g->y1 - g->y0;
        float d = sqrtf(dx*dx + dy*dy);
        float px = x - g->x0, py = y - g->y0;
        float pd = sqrtf(px*px + py*py);
        float r0 = g->r0, r1 = g->r1;
        if(fabsf(r1 - r0) < 1e-6f) t = (pd <= r0) ? 0.0f : 1.0f;
        else t = (pd - r0) / (r1 - r0);
        (void)d;
    } else { /* conic */
        float ang = atan2f(y - g->cy, x - g->cx) - g->angle;
        while(ang < 0) ang += CV_2PI;
        while(ang >= CV_2PI) ang -= CV_2PI;
        t = ang / CV_2PI;
    }
    if(t < 0.0f) t = 0.0f;
    if(t > 1.0f) t = 1.0f;
    /* Find the two surrounding stops. */
    if(g->stops_n == 1) return g->stops[0].color;
    if(t <= g->stops[0].offset) return g->stops[0].color;
    if(t >= g->stops[g->stops_n-1].offset) return g->stops[g->stops_n-1].color;
    for(int i = 1; i < g->stops_n; ++i) {
        if(t <= g->stops[i].offset) {
            float o0 = g->stops[i-1].offset, o1 = g->stops[i].offset;
            float u = (o1 > o0) ? (t - o0) / (o1 - o0) : 0.0f;
            return color_lerp(g->stops[i-1].color, g->stops[i].color, u);
        }
    }
    return g->stops[g->stops_n-1].color;
}

/* Fill one horizontal span [xa,xb) at y with the current style. For solid
 * colors this is one fill_rect; for gradients it samples per-pixel; for
 * patterns it blits tiles. */
static void cv_fill_span(js_canvas_state* st, js_canvas_ctx* cv,
                         int xa, int xb, int y, const cv_style* style) {
    if(xb <= xa) return;
    if(style->kind == 0) {
        uint32_t c = color_scale_alpha(style->color, cv->globalAlpha);
        if(c == 0) return;
        if(st->cb.fill_rect != NULL)
            st->cb.fill_rect(st->ctx, cv->canvas, xa, y, xb - xa, 1, c);
        return;
    }
    if(style->kind == 1 && style->obj != NULL) {
        cv_gradient* g = (cv_gradient*)style->obj;
        /* Sample the gradient once per pixel. For long spans this is slow
         * but correct; a production impl would cache per-scanline. */
        for(int x = xa; x < xb; ++x) {
            uint32_t c = cv_gradient_color(g, (float)x + 0.5f, (float)y + 0.5f);
            c = color_scale_alpha(c, cv->globalAlpha);
            if(c == 0) continue;
            if(st->cb.set_pixel != NULL)
                st->cb.set_pixel(st->ctx, cv->canvas, x, y, c);
            else if(st->cb.fill_rect != NULL)
                st->cb.fill_rect(st->ctx, cv->canvas, x, y, 1, 1, c);
        }
        return;
    }
    if(style->kind == 2 && style->obj != NULL && st->cb.blit != NULL) {
        cv_pattern* p = (cv_pattern*)style->obj;
        if(p->image == NULL) return;
        int pw = 0, ph = 0;
        if(st->cb.bitmap_dims != NULL) st->cb.bitmap_dims(st->ctx, p->image, &pw, &ph);
        if(pw <= 0 || ph <= 0) return;
        int rep_x = (p->repeat == JS_PATTERN_REPEAT || p->repeat == JS_PATTERN_REPEAT_X);
        int rep_y = (p->repeat == JS_PATTERN_REPEAT || p->repeat == JS_PATTERN_REPEAT_Y);
        int sx0 = xa, sx1 = xb;
        if(!rep_x) { if(sx0 < 0) sx0 = 0; if(sx1 > pw) sx1 = pw; }
        if(sx1 <= sx0) return;
        /* Tile horizontally. */
        for(int tx = sx0; tx < sx1; ) {
            int sw = pw - ((tx % pw) + pw) % pw;
            if(sw <= 0) sw = pw;
            if(tx + sw > sx1) sw = sx1 - tx;
            if(sw <= 0) break;
            int src_x = ((tx % pw) + pw) % pw;
            st->cb.blit(st->ctx, cv->canvas, p->image,
                        src_x, ((y % ph) + ph) % ph, sw, 1,
                        tx, y, sw, 1,
                        (uint8_t)(cv->globalAlpha * 255.0f + 0.5f),
                        cv->imageSmoothingEnabled);
            tx += sw;
            if(!rep_x) break;
        }
        (void)rep_y;
        return;
    }
}

/* Even-odd scanline fill of one closed polygon (device space). offx/offy is
 * a flat device-space shift applied to every emitted span (used for the
 * shadow pass; 0,0 for normal painting). */
static void fill_polygon_solid(js_canvas_state* st, js_canvas_ctx* cv,
                               const cv_point_t* pts, int n, const cv_style* style,
                               int offx, int offy) {
    if(cv == NULL || cv->canvas == NULL || pts == NULL || n < 3) return;
    float miny = pts[0].y, maxy = pts[0].y;
    for(int i = 1; i < n; i++) {
        if(pts[i].y < miny) miny = pts[i].y;
        if(pts[i].y > maxy) maxy = pts[i].y;
    }
    int y0 = (int)floorf(miny) + offy; if(y0 < 0) y0 = 0;
    int y1 = (int)ceilf(maxy) + offy;
    if(cv->has_clip) {
        if(y0 < cv->clip_y) y0 = cv->clip_y;
        if(y1 >= cv->clip_y + cv->clip_h) y1 = cv->clip_y + cv->clip_h - 1;
    }
    for(int y = y0; y <= y1; y++) {
        float fy = (float)(y - offy) + 0.5f;
        cv->xs_n = 0;
        for(int i = 0; i < n; i++) {
            const cv_point_t* p1 = &pts[i];
            const cv_point_t* p2 = &pts[(i + 1) % n];
            if((p1->y <= fy && p2->y > fy) || (p2->y <= fy && p1->y > fy)) {
                float t = (fy - p1->y) / (p2->y - p1->y);
                float xv = p1->x + t * (p2->x - p1->x);
                cv->xs = (float*)cv_grow(cv->xs, &cv->xs_cap, cv->xs_n + 1, sizeof(float));
                if(cv->xs_n + 1 > cv->xs_cap) break;
                cv->xs[cv->xs_n++] = xv;
            }
        }
        for(int i = 1; i < cv->xs_n; i++) {   /* insertion sort */
            float v = cv->xs[i]; int j = i;
            while(j > 0 && cv->xs[j - 1] > v) { cv->xs[j] = cv->xs[j - 1]; j--; }
            cv->xs[j] = v;
        }
        for(int i = 0; i + 1 < cv->xs_n; i += 2) {
            int xa = (int)(cv->xs[i] + 0.5f) + offx;
            int xb = (int)(cv->xs[i + 1] + 0.5f) + offx;
            if(cv->has_clip) {
                if(xa < cv->clip_x) xa = cv->clip_x;
                if(xb > cv->clip_x + cv->clip_w) xb = cv->clip_x + cv->clip_w;
            }
            if(xb > xa) cv_fill_span(st, cv, xa, xb, y, style);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Stroke helpers (device space)                                      */
/* ------------------------------------------------------------------ */

/* Draw one dashed segment of a polyline. The dash pattern is in device
 * space; `offset` is the running position along the path. */
static void stroke_dashed_seg(js_canvas_state* st, js_canvas_ctx* cv,
                              float x0, float y0, float x1, float y1,
                              float* offset, uint32_t color, int lw) {
    if(cv->dash_n == 0) {
        if(st->cb.draw_line != NULL)
            st->cb.draw_line(st->ctx, cv->canvas,
                             (int)(x0+0.5f), (int)(y0+0.5f),
                             (int)(x1+0.5f), (int)(y1+0.5f), lw, color);
        return;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx*dx + dy*dy);
    if(len <= 0.0f) return;
    float ux = dx / len, uy = dy / len;
    float pos = 0.0f;
    int di = 0;
    /* Find the starting dash index from *offset. */
    float total = 0.0f;
    for(int i = 0; i < cv->dash_n; ++i) total += cv->dash[i];
    if(total <= 0.0f) {
        if(st->cb.draw_line != NULL)
            st->cb.draw_line(st->ctx, cv->canvas,
                             (int)(x0+0.5f), (int)(y0+0.5f),
                             (int)(x1+0.5f), (int)(y1+0.5f), lw, color);
        return;
    }
    float o = fmodf(*offset + cv->lineDashOffset, total);
    if(o < 0) o += total;
    while(o > cv->dash[di]) { o -= cv->dash[di]; di = (di + 1) % cv->dash_n; }
    int drawing = (di % 2 == 0);
    float remaining = cv->dash[di] - o;
    while(pos < len) {
        float step = len - pos;
        if(step > remaining) step = remaining;
        if(drawing && step > 0.0f) {
            float sx = x0 + ux * pos, sy = y0 + uy * pos;
            float ex = x0 + ux * (pos + step), ey = y0 + uy * (pos + step);
            if(st->cb.draw_line != NULL)
                st->cb.draw_line(st->ctx, cv->canvas,
                                 (int)(sx+0.5f), (int)(sy+0.5f),
                                 (int)(ex+0.5f), (int)(ey+0.5f), lw, color);
        }
        pos += step;
        remaining -= step;
        if(remaining <= 0.0001f) {
            di = (di + 1) % cv->dash_n;
            remaining = cv->dash[di];
            drawing = !drawing;
        }
    }
    *offset += len;
}

/* Stroke one subpath as a polyline (device space). Round caps dot the two
 * ends of an open path; round/bevel joins are approximated by drawing a
 * filled circle / nothing at each interior vertex. offx/offy is a flat
 * device-space shift (shadow pass; 0,0 normally). */
static void stroke_subpath(js_canvas_state* st, js_canvas_ctx* cv,
                           const cv_point_t* pts, int n, int closed,
                           uint32_t color, float lw, int cap, int join,
                           int offx, int offy) {
    if(cv == NULL || cv->canvas == NULL || pts == NULL || n < 1) return;
    int ilw = (int)(lw + 0.5f); if(ilw < 1) ilw = 1;
    float dash_offset = 0.0f;
    int segs = closed ? n : (n - 1);
    for(int i = 0; i < segs; i++) {
        const cv_point_t* p1 = &pts[i];
        const cv_point_t* p2 = &pts[(i + 1) % n];
        stroke_dashed_seg(st, cv, p1->x + offx, p1->y + offy, p2->x + offx, p2->y + offy, &dash_offset, color, ilw);
        /* Joins: draw a filled circle at interior vertices for round join,
         * nothing for bevel (the two segments already meet), and for miter
         * the graph library's wline already extends the join. */
        if(join == JS_LINE_JOIN_ROUND && i < segs - 1) {
            int rad = ilw / 2; if(rad < 1) rad = 1;
            if(st->cb.fill_circle != NULL)
                st->cb.fill_circle(st->ctx, cv->canvas,
                                   (int)(p2->x+0.5f)+offx, (int)(p2->y+0.5f)+offy, rad, color);
        }
    }
    /* Caps on open paths. */
    if(!closed && n >= 1) {
        if(cap == JS_LINE_CAP_ROUND && st->cb.fill_circle != NULL) {
            int rad = ilw / 2; if(rad < 1) rad = 1;
            st->cb.fill_circle(st->ctx, cv->canvas,
                               (int)(pts[0].x+0.5f)+offx, (int)(pts[0].y+0.5f)+offy, rad, color);
            st->cb.fill_circle(st->ctx, cv->canvas,
                               (int)(pts[n-1].x+0.5f)+offx, (int)(pts[n-1].y+0.5f)+offy, rad, color);
        } else if(cap == JS_LINE_CAP_SQUARE) {
            /* Extend each end by lw/2 along the tangent. */
            int half = ilw / 2;
            if(n >= 2) {
                float dx = pts[1].x - pts[0].x, dy = pts[1].y - pts[0].y;
                float d = sqrtf(dx*dx + dy*dy);
                if(d > 0.0f && st->cb.draw_line != NULL) {
                    float ex = pts[0].x - dx/d * half, ey = pts[0].y - dy/d * half;
                    st->cb.draw_line(st->ctx, cv->canvas,
                                     (int)(pts[0].x+0.5f)+offx, (int)(pts[0].y+0.5f)+offy,
                                     (int)(ex+0.5f)+offx, (int)(ey+0.5f)+offy, ilw, color);
                }
                dx = pts[n-1].x - pts[n-2].x; dy = pts[n-1].y - pts[n-2].y;
                d = sqrtf(dx*dx + dy*dy);
                if(d > 0.0f && st->cb.draw_line != NULL) {
                    float ex = pts[n-1].x + dx/d * half, ey = pts[n-1].y + dy/d * half;
                    st->cb.draw_line(st->ctx, cv->canvas,
                                     (int)(pts[n-1].x+0.5f)+offx, (int)(pts[n-1].y+0.5f)+offy,
                                     (int)(ex+0.5f)+offx, (int)(ey+0.5f)+offy, ilw, color);
                }
            }
        }
    }
}

/* Apply the shadow (offset + color) before a paint op. The graph library has
 * no blur primitive, so shadowBlur is recorded but rendered as a hard offset
 * draw; a future embedder could swap in a real gaussian. The shadow offset is
 * in device space and is NOT affected by the CTM (per spec), so it is applied
 * as a flat (offx, offy) shift on every emitted device coordinate. */


/* ------------------------------------------------------------------ */
/* Style release / assign helpers                                     */
/* ------------------------------------------------------------------ */

static void cv_style_release(js_canvas_state* st, cv_style* s) {
    if(s == NULL || s->kind == 0 || s->obj == NULL) { s->kind = 0; s->obj = NULL; return; }
    if(s->kind == 1) {
        cv_gradient* g = (cv_gradient*)s->obj;
        g->refcount--;
        if(g->refcount <= 0) {
            /* Remove from registry and free. */
            if(st != NULL) {
                for(int i = 0; i < st->grads_n; ++i) {
                    if(st->grads[i] == g) {
                        st->grads[i] = st->grads[st->grads_n - 1];
                        st->grads_n--;
                        break;
                    }
                }
            }
            cv_grad_free(g);
        }
    } else if(s->kind == 2) {
        cv_pattern* p = (cv_pattern*)s->obj;
        p->refcount--;
        if(p->refcount <= 0) {
            if(st != NULL) {
                for(int i = 0; i < st->pats_n; ++i) {
                    if(st->pats[i] == p) {
                        st->pats[i] = st->pats[st->pats_n - 1];
                        st->pats_n--;
                        break;
                    }
                }
            }
            cv_pat_free(p);
        }
    }
    s->kind = 0; s->obj = NULL; s->color = 0xFF000000u;
}

static void cv_style_set_color(cv_style* s, uint32_t c) {
    s->kind = 0; s->color = c; s->obj = NULL;
}

static void cv_style_set_obj(js_canvas_state* st, cv_style* s, int kind, void* obj) {
    cv_style_release(st, s);
    s->kind = kind;
    s->obj = obj;
    if(kind == 1) ((cv_gradient*)obj)->refcount++;
    else if(kind == 2) ((cv_pattern*)obj)->refcount++;
}

/* Resolve the effective paint color for a style at device-space (x,y).
 * For solid colors this is just the color; for gradients it samples; for
 * patterns it returns 0 (the caller must use cv_fill_span / blit instead). */
static uint32_t cv_style_color_at(const cv_style* s, float x, float y) {
    if(s->kind == 0) return s->color;
    if(s->kind == 1) return cv_gradient_color((const cv_gradient*)s->obj, x, y);
    return 0xFF000000u;   /* pattern: caller handles separately */
}

/* ------------------------------------------------------------------ */
/* Path construction natives                                          */
/* ------------------------------------------------------------------ */

static var_t* js_ctx_beginPath(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    cv->pts_n = 0; cv->subs_n = 0;
    return NULL;
}

static var_t* js_ctx_closePath(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    cv_close_sub(cv);
    return NULL;
}

static var_t* js_ctx_moveTo(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    cv_begin_sub(cv, get_float(env, "x"), get_float(env, "y"));
    return NULL;
}

static var_t* js_ctx_lineTo(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    cv_add_pt(cv, get_float(env, "x"), get_float(env, "y"));
    return NULL;
}

static var_t* js_ctx_quadraticCurveTo(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float cpx = get_float(env, "cpx"), cpy = get_float(env, "cpy");
    float x = get_float(env, "x"), y = get_float(env, "y");
    /* Pen position: last point of the current subpath (device space) -> we
     * need user space for tessellation. Recover by inverse-transforming the
     * last device point. */
    float p0x = 0, p0y = 0;
    int pure;
    if(cv->subs_n > 0) {
        cv_sub_t* s = &cv->subs[cv->subs_n - 1];
        if(s->pts_n > 0) {
            cv_point_t* lp = &cv->pts[cv->pts_n - 1];
            float det = cv_det(cv);
            if(fabsf(det) > 1e-9f) {
                float dx = lp->x - cv->e, dy = lp->y - cv->f;
                p0x = ( dx * cv->d - dy * cv->c) / det;
                p0y = (-dx * cv->b + dy * cv->a) / det;
            } else { p0x = x; p0y = y; }
        }
    } else {
        cv_begin_sub(cv, cpx, cpy);
        p0x = cpx; p0y = cpy;
    }
    pure = (cv->subs_n > 0 && cv->subs[cv->subs_n - 1].pts_n == 1);
    cv_flatten_quadratic(cv, p0x, p0y, cpx, cpy, x, y);
    /* Tag a single-curve subpath (moveTo + one quadraticCurveTo) so stroke()
     * can dispatch straight to graph_quadratic_curve_w and fill() reuses the
     * baked points. Mixed subpaths stay SUB_GENERAL (general polyline). */
    if(pure && cv->subs_n > 0) {
        cv_sub_t* s = &cv->subs[cv->subs_n - 1];
        s->kind = SUB_QUADRATIC;
        s->p[0] = p0x; s->p[1] = p0y;
        s->p[2] = cpx; s->p[3] = cpy;
        s->p[4] = x;   s->p[5] = y;
    }
    return NULL;
}

static var_t* js_ctx_bezierCurveTo(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float c1x = get_float(env, "cp1x"), c1y = get_float(env, "cp1y");
    float c2x = get_float(env, "cp2x"), c2y = get_float(env, "cp2y");
    float x = get_float(env, "x"), y = get_float(env, "y");
    float p0x = 0, p0y = 0;
    int pure;
    if(cv->subs_n > 0 && cv->subs[cv->subs_n-1].pts_n > 0) {
        cv_point_t* lp = &cv->pts[cv->pts_n - 1];
        float det = cv_det(cv);
        if(fabsf(det) > 1e-9f) {
            float dx = lp->x - cv->e, dy = lp->y - cv->f;
            p0x = ( dx * cv->d - dy * cv->c) / det;
            p0y = (-dx * cv->b + dy * cv->a) / det;
        } else { p0x = x; p0y = y; }
    } else {
        cv_begin_sub(cv, c1x, c1y);
        p0x = c1x; p0y = c1y;
    }
    pure = (cv->subs_n > 0 && cv->subs[cv->subs_n - 1].pts_n == 1);
    cv_flatten_cubic(cv, p0x, p0y, c1x, c1y, c2x, c2y, x, y);
    /* Tag a single-curve subpath (moveTo + one bezierCurveTo) so stroke() can
     * dispatch to graph_bezier_curve_w and fill() reuses the baked points. */
    if(pure && cv->subs_n > 0) {
        cv_sub_t* s = &cv->subs[cv->subs_n - 1];
        s->kind = SUB_CUBIC;
        s->p[0] = p0x; s->p[1] = p0y;
        s->p[2] = c1x; s->p[3] = c1y;
        s->p[4] = c2x; s->p[5] = c2y;
        s->p[6] = x;   s->p[7] = y;
    }
    return NULL;
}

static var_t* js_ctx_arcTo(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float x1 = get_float(env, "x1"), y1 = get_float(env, "y1");
    float x2 = get_float(env, "x2"), y2 = get_float(env, "y2");
    float r  = get_float(env, "r");
    if(r < 0 || isnan(r) || isinf(r)) return NULL;
    /* Current pen (user space). */
    float x0 = 0, y0 = 0;
    if(cv->subs_n > 0 && cv->subs[cv->subs_n-1].pts_n > 0) {
        cv_point_t* lp = &cv->pts[cv->pts_n - 1];
        float det = cv_det(cv);
        if(fabsf(det) > 1e-9f) {
            float dx = lp->x - cv->e, dy = lp->y - cv->f;
            x0 = ( dx * cv->d - dy * cv->c) / det;
            y0 = (-dx * cv->b + dy * cv->a) / det;
        }
    } else {
        cv_begin_sub(cv, x1, y1);
        return NULL;
    }
    /* Per spec: if (x0,y0)==(x1,y1) or (x1,y1)==(x2,y2) or r==0, lineTo(x1,y1). */
    if((fabsf(x0-x1) < 1e-6f && fabsf(y0-y1) < 1e-6f) ||
       (fabsf(x1-x2) < 1e-6f && fabsf(y1-y2) < 1e-6f) || r == 0.0f) {
        cv_add_pt(cv, x1, y1);
        return NULL;
    }
    /* Compute the arc center and tangent points. */
    float v0x = x0 - x1, v0y = y0 - y1;
    float v2x = x2 - x1, v2y = y2 - y1;
    float n0 = sqrtf(v0x*v0x + v0y*v0y);
    float n2 = sqrtf(v2x*v2x + v2y*v2y);
    if(n0 <= 0 || n2 <= 0) { cv_add_pt(cv, x1, y1); return NULL; }
    v0x /= n0; v0y /= n0; v2x /= n2; v2y /= n2;
    float cross = v0x * v2y - v0y * v2x;
    float dot = v0x * v2x + v0y * v2y;
    if(fabsf(cross) < 1e-6f) { cv_add_pt(cv, x1, y1); return NULL; }
    /* Half-angle between the two directions. */
    float ang = acosf(fmaxf(-1.0f, fminf(1.0f, dot)));
    float t = r / tanf(ang * 0.5f);
    /* Tangent points. */
    float t0x = x1 + v0x * t, t0y = y1 + v0y * t;
    float t2x = x1 + v2x * t, t2y = y1 + v2y * t;
    /* Center: offset from x1 along the angle bisector, distance r/sin(ang/2). */
    float bisx = v0x + v2x, bisy = v0y + v2y;
    float nb = sqrtf(bisx*bisx + bisy*bisy);
    if(nb <= 0) { cv_add_pt(cv, x1, y1); return NULL; }
    bisx /= nb; bisy /= nb;
    float cd = r / sinf(ang * 0.5f);
    float cx = x1 + bisx * cd, cy = y1 + bisy * cd;
    /* Start/end angles from center to tangent points. */
    float a0 = atan2f(t0y - cy, t0x - cx);
    float a1 = atan2f(t2y - cy, t2x - cx);
    int ccw = (cross > 0) ? 1 : 0;
    /* Line to the first tangent point, then arc to the second. */
    cv_add_pt(cv, t0x, t0y);
    cv_tess_ellipse(cv, cx, cy, r, r, 0.0f, a0, a1, ccw, 0);
    return NULL;
}

static var_t* js_ctx_rect(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float x = get_float(env, "x"), y = get_float(env, "y");
    float w = get_float(env, "w"), h = get_float(env, "h");
    /* Per spec: moveTo(x,y), lineTo(x+w,y), lineTo(x+w,y+h), lineTo(x,y+h), closePath. */
    cv_begin_sub_kind(cv, x, y, SUB_RECT);
    float p[4] = {x, y, w, h};
    cv_set_sub_params(cv, p, 4);
    cv_add_pt(cv, x + w, y);
    cv_add_pt(cv, x + w, y + h);
    cv_add_pt(cv, x, y + h);
    cv_close_sub(cv);
    /* Restore the kind (cv_add_pt downgraded it). */
    if(cv->subs_n > 0) {
        cv->subs[cv->subs_n-1].kind = SUB_RECT;
        cv->subs[cv->subs_n-1].p[0]=x; cv->subs[cv->subs_n-1].p[1]=y;
        cv->subs[cv->subs_n-1].p[2]=w; cv->subs[cv->subs_n-1].p[3]=h;
    }
    return NULL;
}

static var_t* js_ctx_roundRect(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float x = get_float(env, "x"), y = get_float(env, "y");
    float w = get_float(env, "w"), h = get_float(env, "h");
    /* radii: a single number or an array of 1..4. mario passes the first arg
     * as "r"; for the array form we read it as a float (degenerate but works
     * for the common single-radius case). */
    float r = get_float(env, "r");
    if(r < 0) r = 0;
    if(r > w * 0.5f) r = w * 0.5f;
    if(r > h * 0.5f) r = h * 0.5f;
    cv_begin_sub_kind(cv, x + r, y, SUB_ROUND_RECT);
    float p[5] = {x, y, w, h, r};
    cv_set_sub_params(cv, p, 5);
    /* Build with lineTo + arcTo for each corner. */
    cv_add_pt(cv, x + w - r, y);
    cv_tess_ellipse(cv, x + w - r, y + r, r, r, 0, -CV_PI/2, 0, 0, 0);
    cv_add_pt(cv, x + w, y + h - r);
    cv_tess_ellipse(cv, x + w - r, y + h - r, r, r, 0, 0, CV_PI/2, 0, 0);
    cv_add_pt(cv, x + r, y + h);
    cv_tess_ellipse(cv, x + r, y + h - r, r, r, 0, CV_PI/2, CV_PI, 0, 0);
    cv_add_pt(cv, x, y + r);
    cv_tess_ellipse(cv, x + r, y + r, r, r, 0, CV_PI, CV_PI*1.5f, 0, 0);
    cv_close_sub(cv);
    if(cv->subs_n > 0) {
        cv->subs[cv->subs_n-1].kind = SUB_ROUND_RECT;
        for(int i = 0; i < 5; ++i) cv->subs[cv->subs_n-1].p[i] = p[i];
    }
    return NULL;
}

static var_t* js_ctx_arc(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float cx = get_float(env, "x"), cy = get_float(env, "y"), r = get_float(env, "r");
    float a0 = get_float(env, "start"), a1 = get_float(env, "end");
    /* counterclockwise is the 6th arg; mario's decl only names the first 5,
     * so read it from the args array. */
    var_t* args = get_func_args(env);
    node_t* ccwn = var_array_get(args, 5);
    int ccw = 0;
    if(ccwn != NULL && ccwn->var != NULL) {
        const char* s = var_get_str(ccwn->var);
        if(s != NULL && (!strcasecmp(s, "true") || !strcmp(s, "1"))) ccw = 1;
        else if(var_get_int(ccwn->var) != 0) ccw = 1;
    }
    if(r < 0 || isnan(r) || isinf(r)) return NULL;
    if(r == 0.0f) return NULL;
    int move_first = (cv->subs_n == 0);
    cv_tess_ellipse(cv, cx, cy, r, r, 0.0f, a0, a1, ccw, move_first);
    /* Tag the subpath as an arc (or circle if full sweep) for fast dispatch. */
    if(cv->subs_n > 0) {
        cv_sub_t* s = &cv->subs[cv->subs_n - 1];
        float sweep = a1 - a0;
        if(ccw) sweep = -sweep;
        while(sweep < 0) sweep += CV_2PI;
        if(sweep > CV_2PI) sweep = CV_2PI;
        if(fabsf(sweep - CV_2PI) < 1e-3f) {
            s->kind = SUB_CIRCLE;
            s->p[0] = cx; s->p[1] = cy; s->p[2] = r;
            s->closed = 1;
        } else {
            s->kind = SUB_ARC;
            s->p[0] = cx; s->p[1] = cy; s->p[2] = r;
            s->p[3] = a0; s->p[4] = a1; s->p[5] = (float)ccw;
        }
    }
    return NULL;
}

static var_t* js_ctx_ellipse(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float cx = get_float(env, "x"), cy = get_float(env, "y");
    float rx = get_float(env, "rx"), ry = get_float(env, "ry");
    float rot = get_float(env, "rot");
    float a0 = get_float(env, "start"), a1 = get_float(env, "end");
    var_t* args = get_func_args(env);
    node_t* ccwn = var_array_get(args, 7);
    int ccw = 0;
    if(ccwn != NULL && ccwn->var != NULL) {
        const char* s = var_get_str(ccwn->var);
        if(s != NULL && (!strcasecmp(s, "true") || !strcmp(s, "1"))) ccw = 1;
        else if(var_get_int(ccwn->var) != 0) ccw = 1;
    }
    if(rx < 0 || ry < 0 || isnan(rx) || isnan(ry) || isinf(rx) || isinf(ry)) return NULL;
    if(rx == 0 || ry == 0) return NULL;
    int move_first = (cv->subs_n == 0);
    cv_tess_ellipse(cv, cx, cy, rx, ry, rot, a0, a1, ccw, move_first);
    if(cv->subs_n > 0) {
        cv_sub_t* s = &cv->subs[cv->subs_n - 1];
        s->kind = SUB_ELLIPSE;
        s->p[0]=cx; s->p[1]=cy; s->p[2]=rx; s->p[3]=ry;
        s->p[4]=rot; s->p[5]=a0; s->p[6]=a1; s->p[7]=(float)ccw;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Paint natives                                                      */
/* ------------------------------------------------------------------ */

/* The graph library has no blur primitive, so shadowBlur is accepted but
 * ignored (it still round-trips through save/restore and getContextAttributes).
 * Shadows are drawn as a hard device-space offset pre-pass: the same shape
 * is painted again with shadowColor shifted by (shadowOffsetX,Y) before the
 * main paint. offx/offy are threaded through cv_fill_path/cv_stroke_path so
 * both the fast paths (which re-transform user-space params) and the generic
 * scanline/stroke paths (which use baked device-space points) shift together.
 * Returns 0 when no shadow pass is required. */
static int cv_shadow_offsets(const js_canvas_ctx* cv, int* offx, int* offy) {
    if(cv == NULL || cv->shadowColor == 0) return 0;
    if(cv->shadowOffsetX == 0.0f && cv->shadowOffsetY == 0.0f) return 0;
    *offx = (int)(cv->shadowOffsetX + (cv->shadowOffsetX >= 0 ? 0.5f : -0.5f));
    *offy = (int)(cv->shadowOffsetY + (cv->shadowOffsetY >= 0 ? 0.5f : -0.5f));
    return 1;
}

/* Paint the current path with `style`. Dispatches to the fast path for
 * typed subpaths (rect/round-rect/circle/arc) and falls back to scanline
 * fill for general polygons. (offx,offy) is a device-space shift applied
 * to every emitted pixel; (0,0) for the normal pass, shadow offsets for
 * the shadow pre-pass. */
static void cv_fill_path(js_canvas_state* st, js_canvas_ctx* cv,
                         const cv_style* style, int offx, int offy) {
    if(cv == NULL || cv->canvas == NULL || st == NULL) return;
    for(int s = 0; s < cv->subs_n; ++s) {
        cv_sub_t* sub = &cv->subs[s];
        if(sub->pts_n < 3 && sub->kind != SUB_CIRCLE) continue;
        const cv_point_t* pts = &cv->pts[sub->pts_start];
        int n = sub->pts_n;
        /* Fast paths only work for solid colors; gradients/patterns always
         * go through the scanline span painter. */
        if(style->kind == 0) {
            uint32_t c = color_scale_alpha(style->color, cv->globalAlpha);
            if(c == 0) continue;
            if(sub->kind == SUB_RECT && st->cb.fill_rect != NULL) {
                float x0,y0,x1,y1;
                cv_xform(cv, sub->p[0], sub->p[1], &x0, &y0);
                cv_xform(cv, sub->p[0]+sub->p[2], sub->p[1]+sub->p[3], &x1, &y1);
                int ix = (int)(fminf(x0,x1)+0.5f) + offx;
                int iy = (int)(fminf(y0,y1)+0.5f) + offy;
                int jx = (int)(fmaxf(x0,x1)+0.5f) + offx;
                int jy = (int)(fmaxf(y0,y1)+0.5f) + offy;
                st->cb.fill_rect(st->ctx, cv->canvas, ix, iy, jx-ix, jy-iy, c);
                continue;
            }
            if(sub->kind == SUB_CIRCLE && st->cb.fill_circle != NULL) {
                float dx, dy;
                cv_xform(cv, sub->p[0], sub->p[1], &dx, &dy);
                int r = (int)(sub->p[2] * cv_scale(cv) + 0.5f);
                st->cb.fill_circle(st->ctx, cv->canvas,
                                   (int)(dx+0.5f) + offx, (int)(dy+0.5f) + offy, r, c);
                continue;
            }
            if(sub->kind == SUB_ARC && st->cb.fill_arc != NULL) {
                float dx, dy;
                cv_xform(cv, sub->p[0], sub->p[1], &dx, &dy);
                int r = (int)(sub->p[2] * cv_scale(cv) + 0.5f);
                st->cb.fill_arc(st->ctx, cv->canvas,
                                (int)(dx+0.5f) + offx, (int)(dy+0.5f) + offy, r,
                                sub->p[3], sub->p[4], c);
                continue;
            }
            if(sub->kind == SUB_ROUND_RECT && st->cb.fill_round_rect != NULL) {
                float x0,y0,x1,y1;
                cv_xform(cv, sub->p[0], sub->p[1], &x0, &y0);
                cv_xform(cv, sub->p[0]+sub->p[2], sub->p[1]+sub->p[3], &x1, &y1);
                int ix = (int)(fminf(x0,x1)+0.5f) + offx;
                int iy = (int)(fminf(y0,y1)+0.5f) + offy;
                int jx = (int)(fmaxf(x0,x1)+0.5f) + offx;
                int jy = (int)(fmaxf(y0,y1)+0.5f) + offy;
                int r = (int)(sub->p[4] * cv_scale(cv) + 0.5f);
                st->cb.fill_round_rect(st->ctx, cv->canvas, ix, iy, jx-ix, jy-iy, r, c);
                continue;
            }
        }
        /* Generic: scanline fill with per-span style resolution. */
        fill_polygon_solid(st, cv, pts, n, style, offx, offy);
    }
}

/* Stroke the current path. (offx,offy) is the shadow device-space shift
 * (0,0 for the normal pass). */
static void cv_stroke_path(js_canvas_state* st, js_canvas_ctx* cv,
                           const cv_style* style, int offx, int offy) {
    if(cv == NULL || cv->canvas == NULL || st == NULL) return;
    float lw = cv->lineWidth * cv_scale(cv);
    if(lw < 1.0f) lw = 1.0f;
    for(int s = 0; s < cv->subs_n; ++s) {
        cv_sub_t* sub = &cv->subs[s];
        int n = sub->pts_n;
        if(n < 1) continue;
        const cv_point_t* pts = &cv->pts[sub->pts_start];
        /* Fast paths for solid colors. */
        if(style->kind == 0) {
            uint32_t c = color_scale_alpha(style->color, cv->globalAlpha);
            if(c == 0) continue;
            if(sub->kind == SUB_CIRCLE && st->cb.stroke_circle != NULL) {
                float dx, dy;
                cv_xform(cv, sub->p[0], sub->p[1], &dx, &dy);
                int r = (int)(sub->p[2] * cv_scale(cv) + 0.5f);
                st->cb.stroke_circle(st->ctx, cv->canvas,
                                     (int)(dx+0.5f) + offx, (int)(dy+0.5f) + offy,
                                     r, (int)(lw+0.5f), c);
                continue;
            }
            if(sub->kind == SUB_ARC && st->cb.stroke_arc != NULL) {
                float dx, dy;
                cv_xform(cv, sub->p[0], sub->p[1], &dx, &dy);
                int r = (int)(sub->p[2] * cv_scale(cv) + 0.5f);
                st->cb.stroke_arc(st->ctx, cv->canvas,
                                  (int)(dx+0.5f) + offx, (int)(dy+0.5f) + offy,
                                  r, (int)(lw+0.5f), sub->p[3], sub->p[4], c);
                continue;
            }
            if(sub->kind == SUB_RECT && st->cb.stroke_rect != NULL) {
                float x0,y0,x1,y1;
                cv_xform(cv, sub->p[0], sub->p[1], &x0, &y0);
                cv_xform(cv, sub->p[0]+sub->p[2], sub->p[1]+sub->p[3], &x1, &y1);
                int ix = (int)(fminf(x0,x1)+0.5f) + offx;
                int iy = (int)(fminf(y0,y1)+0.5f) + offy;
                int jx = (int)(fmaxf(x0,x1)+0.5f) + offx;
                int jy = (int)(fmaxf(y0,y1)+0.5f) + offy;
                st->cb.stroke_rect(st->ctx, cv->canvas, ix, iy, jx-ix, jy-iy,
                                   (int)(lw+0.5f), c);
                continue;
            }
            if(sub->kind == SUB_ROUND_RECT && st->cb.stroke_round_rect != NULL) {
                float x0,y0,x1,y1;
                cv_xform(cv, sub->p[0], sub->p[1], &x0, &y0);
                cv_xform(cv, sub->p[0]+sub->p[2], sub->p[1]+sub->p[3], &x1, &y1);
                int ix = (int)(fminf(x0,x1)+0.5f) + offx;
                int iy = (int)(fminf(y0,y1)+0.5f) + offy;
                int jx = (int)(fmaxf(x0,x1)+0.5f) + offx;
                int jy = (int)(fmaxf(y0,y1)+0.5f) + offy;
                int r = (int)(sub->p[4] * cv_scale(cv) + 0.5f);
                st->cb.stroke_round_rect(st->ctx, cv->canvas, ix, iy, jx-ix, jy-iy,
                                         r, (int)(lw+0.5f), c);
                continue;
            }
            /* Single-curve subpath: hand the control points straight to the
             * graph library's curve stroker (it does the de Casteljau
             * flattening + graph_wline emission). Device space + shadow off. */
            if(sub->kind == SUB_QUADRATIC && st->cb.stroke_quadratic != NULL) {
                float x0,y0,cxp,cyp,x1,y1;
                cv_xform(cv, sub->p[0], sub->p[1], &x0, &y0);
                cv_xform(cv, sub->p[2], sub->p[3], &cxp, &cyp);
                cv_xform(cv, sub->p[4], sub->p[5], &x1, &y1);
                st->cb.stroke_quadratic(st->ctx, cv->canvas,
                                        (int)(x0+0.5f)+offx,  (int)(y0+0.5f)+offy,
                                        (int)(cxp+0.5f)+offx, (int)(cyp+0.5f)+offy,
                                        (int)(x1+0.5f)+offx,  (int)(y1+0.5f)+offy,
                                        (int)(lw+0.5f), c);
                continue;
            }
            if(sub->kind == SUB_CUBIC && st->cb.stroke_bezier != NULL) {
                float x0,y0,c1x,c1y,c2x,c2y,x1,y1;
                cv_xform(cv, sub->p[0], sub->p[1], &x0, &y0);
                cv_xform(cv, sub->p[2], sub->p[3], &c1x, &c1y);
                cv_xform(cv, sub->p[4], sub->p[5], &c2x, &c2y);
                cv_xform(cv, sub->p[6], sub->p[7], &x1, &y1);
                st->cb.stroke_bezier(st->ctx, cv->canvas,
                                     (int)(x0+0.5f)+offx,  (int)(y0+0.5f)+offy,
                                     (int)(c1x+0.5f)+offx, (int)(c1y+0.5f)+offy,
                                     (int)(c2x+0.5f)+offx, (int)(c2y+0.5f)+offy,
                                     (int)(x1+0.5f)+offx,  (int)(y1+0.5f)+offy,
                                     (int)(lw+0.5f), c);
                continue;
            }
        }
        /* Generic polyline stroke. For gradients we approximate by sampling
         * the gradient at each segment midpoint. */
        if(style->kind == 1) {
            for(int i = 0; i + 1 < n; ++i) {
                float mx = (pts[i].x + pts[i+1].x) * 0.5f;
                float my = (pts[i].y + pts[i+1].y) * 0.5f;
                uint32_t c = color_scale_alpha(cv_style_color_at(style, mx, my), cv->globalAlpha);
                if(c == 0) continue;
                if(st->cb.draw_line != NULL)
                    st->cb.draw_line(st->ctx, cv->canvas,
                                     (int)(pts[i].x+0.5f) + offx,
                                     (int)(pts[i].y+0.5f) + offy,
                                     (int)(pts[i+1].x+0.5f) + offx,
                                     (int)(pts[i+1].y+0.5f) + offy,
                                     (int)(lw+0.5f), c);
            }
        } else {
            uint32_t c = color_scale_alpha(style->color, cv->globalAlpha);
            stroke_subpath(st, cv, pts, n, sub->closed, c, lw,
                           cv->lineCap, cv->lineJoin, offx, offy);
        }
    }
}

static var_t* js_ctx_fill(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    /* fillRule arg ("nonzero"|"evenodd") is accepted but the scanline fill
     * is even-odd; nonzero would require winding-number tracking. */
    int sx = 0, sy = 0;
    if(cv_shadow_offsets(cv, &sx, &sy)) {
        cv_style sh; sh.kind = 0; sh.color = cv->shadowColor; sh.obj = NULL;
        cv_fill_path(st, cv, &sh, sx, sy);
    }
    cv_fill_path(st, cv, &cv->fillStyle, 0, 0);
    return NULL;
}

static var_t* js_ctx_stroke(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    int sx = 0, sy = 0;
    if(cv_shadow_offsets(cv, &sx, &sy)) {
        cv_style sh; sh.kind = 0; sh.color = cv->shadowColor; sh.obj = NULL;
        cv_stroke_path(st, cv, &sh, sx, sy);
    }
    cv_stroke_path(st, cv, &cv->strokeStyle, 0, 0);
    return NULL;
}

static var_t* js_ctx_clip(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    /* Compute the device-space bounding box of the current path and
     * intersect with the existing clip. The graph library only supports
     * rectangular clips, so path-accurate clipping is approximated. */
    if(cv->pts_n == 0) return NULL;
    float minx = cv->pts[0].x, maxx = minx;
    float miny = cv->pts[0].y, maxy = miny;
    for(int i = 1; i < cv->pts_n; ++i) {
        if(cv->pts[i].x < minx) minx = cv->pts[i].x;
        if(cv->pts[i].x > maxx) maxx = cv->pts[i].x;
        if(cv->pts[i].y < miny) miny = cv->pts[i].y;
        if(cv->pts[i].y > maxy) maxy = cv->pts[i].y;
    }
    int x = (int)floorf(minx), y = (int)floorf(miny);
    int w = (int)ceilf(maxx) - x, h = (int)ceilf(maxy) - y;
    if(w <= 0 || h <= 0) return NULL;
    if(cv->has_clip) {
        int cx0 = cv->clip_x, cy0 = cv->clip_y;
        int cx1 = cx0 + cv->clip_w, cy1 = cy0 + cv->clip_h;
        if(x < cx0) x = cx0;
        if(y < cy0) y = cy0;
        if(x + w > cx1) w = cx1 - x;
        if(y + h > cy1) h = cy1 - y;
        if(w <= 0 || h <= 0) { cv->has_clip = 0; return NULL; }
    }
    cv->clip_x = x; cv->clip_y = y; cv->clip_w = w; cv->clip_h = h;
    cv->has_clip = 1;
    if(st != NULL && st->cb.set_clip != NULL)
        st->cb.set_clip(st->ctx, cv->canvas, x, y, w, h);
    return NULL;
}

/* Even-odd point-in-polygon test (device space). */
static int cv_point_in_poly(const cv_point_t* pts, int n, float x, float y) {
    int inside = 0;
    for(int i = 0, j = n - 1; i < n; j = i++) {
        float xi = pts[i].x, yi = pts[i].y;
        float xj = pts[j].x, yj = pts[j].y;
        if(((yi > y) != (yj > y)) &&
           (x < (xj - xi) * (y - yi) / (yj - yi + 1e-9f) + xi))
            inside = !inside;
    }
    return inside;
}

static var_t* js_ctx_isPointInPath(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return var_new_bool(vm, false);
    float ux = get_float(env, "x"), uy = get_float(env, "y");
    float dx, dy; cv_xform(cv, ux, uy, &dx, &dy);
    int hit = 0;
    for(int s = 0; s < cv->subs_n && !hit; ++s) {
        cv_sub_t* sub = &cv->subs[s];
        if(sub->pts_n < 3) continue;
        if(cv_point_in_poly(&cv->pts[sub->pts_start], sub->pts_n, dx, dy)) hit = 1;
    }
    return var_new_bool(vm, hit != 0);
}

static var_t* js_ctx_isPointInStroke(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return var_new_bool(vm, false);
    float ux = get_float(env, "x"), uy = get_float(env, "y");
    float dx, dy; cv_xform(cv, ux, uy, &dx, &dy);
    float lw = cv->lineWidth * cv_scale(cv) * 0.5f;
    if(lw < 0.5f) lw = 0.5f;
    int hit = 0;
    for(int s = 0; s < cv->subs_n && !hit; ++s) {
        cv_sub_t* sub = &cv->subs[s];
        int n = sub->pts_n;
        if(n < 2) continue;
        const cv_point_t* pts = &cv->pts[sub->pts_start];
        int segs = sub->closed ? n : (n - 1);
        for(int i = 0; i < segs; ++i) {
            float x0 = pts[i].x, y0 = pts[i].y;
            float x1 = pts[(i+1)%n].x, y1 = pts[(i+1)%n].y;
            float vx = x1 - x0, vy = y1 - y0;
            float wx = dx - x0, wy = dy - y0;
            float len2 = vx*vx + vy*vy;
            float t = (len2 > 0) ? (wx*vx + wy*vy) / len2 : 0.0f;
            if(t < 0) t = 0; if(t > 1) t = 1;
            float px = x0 + vx*t, py = y0 + vy*t;
            float ddx = dx - px, ddy = dy - py;
            if(ddx*ddx + ddy*ddy <= lw*lw) { hit = 1; break; }
        }
    }
    return var_new_bool(vm, hit != 0);
}

static var_t* js_ctx_clearRect(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || cv->canvas == NULL || st == NULL || st->cb.fill_rect == NULL) return NULL;
    float x = get_float(env, "x"), y = get_float(env, "y"), w = get_float(env, "w"), h = get_float(env, "h");
    float x0, y0, x1, y1;
    cv_xform(cv, x, y, &x0, &y0);
    cv_xform(cv, x + w, y + h, &x1, &y1);
    int ix = (int)(fminf(x0, x1) + 0.5f), iy = (int)(fminf(y0, y1) + 0.5f);
    int jx = (int)(fmaxf(x0, x1) + 0.5f), jy = (int)(fmaxf(y0, y1) + 0.5f);
    st->cb.fill_rect(st->ctx, cv->canvas, ix, iy, jx - ix, jy - iy, 0x00000000u);
    return NULL;
}

static var_t* js_ctx_fillRect(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || cv->canvas == NULL || st == NULL) return NULL;
    float x = get_float(env, "x"), y = get_float(env, "y"), w = get_float(env, "w"), h = get_float(env, "h");
    if(w == 0 || h == 0) return NULL;
    float x0, y0, x1, y1;
    cv_xform(cv, x, y, &x0, &y0);
    cv_xform(cv, x + w, y + h, &x1, &y1);
    int ix = (int)(fminf(x0, x1) + 0.5f), iy = (int)(fminf(y0, y1) + 0.5f);
    int jx = (int)(fmaxf(x0, x1) + 0.5f), jy = (int)(fmaxf(y0, y1) + 0.5f);
    /* Shadow pre-pass: same rect shifted by (sx,sy) with shadowColor. */
    int sx = 0, sy = 0;
    if(cv_shadow_offsets(cv, &sx, &sy) && st->cb.fill_rect != NULL) {
        uint32_t sc = color_scale_alpha(cv->shadowColor, cv->globalAlpha);
        if(sc != 0)
            st->cb.fill_rect(st->ctx, cv->canvas, ix + sx, iy + sy,
                             jx - ix, jy - iy, sc);
    }
    if(cv->has_clip) {
        if(ix < cv->clip_x) ix = cv->clip_x;
        if(iy < cv->clip_y) iy = cv->clip_y;
        if(jx > cv->clip_x + cv->clip_w) jx = cv->clip_x + cv->clip_w;
        if(jy > cv->clip_y + cv->clip_h) jy = cv->clip_y + cv->clip_h;
    }
    if(jx <= ix || jy <= iy) return NULL;
    if(cv->fillStyle.kind == 0) {
        uint32_t c = color_scale_alpha(cv->fillStyle.color, cv->globalAlpha);
        if(c != 0 && st->cb.fill_rect != NULL)
            st->cb.fill_rect(st->ctx, cv->canvas, ix, iy, jx - ix, jy - iy, c);
    } else {
        /* Gradient/pattern: fill span by span. */
        for(int yy = iy; yy < jy; ++yy)
            cv_fill_span(st, cv, ix, jx, yy, &cv->fillStyle);
    }
    return NULL;
}

static var_t* js_ctx_strokeRect(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || cv->canvas == NULL || st == NULL) return NULL;
    float x = get_float(env, "x"), y = get_float(env, "y"), w = get_float(env, "w"), h = get_float(env, "h");
    float x0, y0, x1, y1;
    cv_xform(cv, x, y, &x0, &y0);
    cv_xform(cv, x + w, y + h, &x1, &y1);
    int ix = (int)(fminf(x0, x1) + 0.5f), iy = (int)(fminf(y0, y1) + 0.5f);
    int jx = (int)(fmaxf(x0, x1) + 0.5f), jy = (int)(fmaxf(y0, y1) + 0.5f);
    int lw = (int)(cv->lineWidth * cv_scale(cv) + 0.5f); if(lw < 1) lw = 1;
    /* Shadow pre-pass. */
    int sx = 0, sy = 0;
    if(cv_shadow_offsets(cv, &sx, &sy) && st->cb.stroke_rect != NULL) {
        uint32_t sc = color_scale_alpha(cv->shadowColor, cv->globalAlpha);
        if(sc != 0)
            st->cb.stroke_rect(st->ctx, cv->canvas, ix + sx, iy + sy,
                               jx - ix, jy - iy, lw, sc);
    }
    uint32_t c = color_scale_alpha(cv_style_color_at(&cv->strokeStyle, (x0+x1)*0.5f, (y0+y1)*0.5f), cv->globalAlpha);
    if(c == 0) return NULL;
    if(st->cb.stroke_rect != NULL)
        st->cb.stroke_rect(st->ctx, cv->canvas, ix, iy, jx - ix, jy - iy, lw, c);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Text natives                                                       */
/* ------------------------------------------------------------------ */

/* Resolve the text drawing origin given align/baseline. Returns the
 * device-space top-left of the text box. */
static void cv_text_origin(js_canvas_state* st, js_canvas_ctx* cv,
                           const char* text, float dx, float dy,
                           int* px, int* py) {
    int tw = 0, th = 0;
    if(st != NULL && st->cb.text_size != NULL)
        st->cb.text_size(st->ctx, cv->canvas, text, cv->fontSize, &tw, &th);
    int x = (int)(dx + 0.5f), y = (int)(dy + 0.5f);
    int align = cv->textAlign;
    if(align == JS_TEXT_ALIGN_START) align = (cv->direction == 1) ? JS_TEXT_ALIGN_RIGHT : JS_TEXT_ALIGN_LEFT;
    else if(align == JS_TEXT_ALIGN_END) align = (cv->direction == 1) ? JS_TEXT_ALIGN_LEFT : JS_TEXT_ALIGN_RIGHT;
    if(align == JS_TEXT_ALIGN_CENTER) x -= tw / 2;
    else if(align == JS_TEXT_ALIGN_RIGHT) x -= tw;
    /* draw_text treats y as the TOP of the ascent line. */
    switch(cv->textBaseline) {
        case JS_TEXT_BASELINE_TOP: break;
        case JS_TEXT_BASELINE_HANGING: y -= (int)(th * 0.2f); break;
        case JS_TEXT_BASELINE_MIDDLE: y -= th / 2; break;
        case JS_TEXT_BASELINE_ALPHABETIC: y -= (int)(th * 0.8f); break;
        case JS_TEXT_BASELINE_IDEOGRAPHIC: y -= th; break;
        case JS_TEXT_BASELINE_BOTTOM: y -= th; break;
    }
    *px = x; *py = y;
}

static var_t* js_ctx_fillText(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || cv->canvas == NULL || st == NULL) return NULL;
    const char* text = get_str(env, "text");
    if(text == NULL || text[0] == 0) return NULL;
    float dx, dy; cv_xform(cv, get_float(env, "x"), get_float(env, "y"), &dx, &dy);
    int px, py; cv_text_origin(st, cv, text, dx, dy, &px, &py);
    /* Shadow pre-pass. */
    int sx = 0, sy = 0;
    if(cv_shadow_offsets(cv, &sx, &sy) && st->cb.draw_text != NULL) {
        uint32_t sc = color_scale_alpha(cv->shadowColor, cv->globalAlpha);
        if(sc != 0)
            st->cb.draw_text(st->ctx, cv->canvas, px + sx, py + sy, text,
                             cv->fontSize, sc);
    }
    uint32_t c = color_scale_alpha(cv_style_color_at(&cv->fillStyle, dx, dy), cv->globalAlpha);
    if(c == 0) return NULL;
    if(st->cb.draw_text != NULL)
        st->cb.draw_text(st->ctx, cv->canvas, px, py, text, cv->fontSize, c);
    return NULL;
}

static var_t* js_ctx_strokeText(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || cv->canvas == NULL || st == NULL) return NULL;
    const char* text = get_str(env, "text");
    if(text == NULL || text[0] == 0) return NULL;
    float dx, dy; cv_xform(cv, get_float(env, "x"), get_float(env, "y"), &dx, &dy);
    int px, py; cv_text_origin(st, cv, text, dx, dy, &px, &py);
    int lw = (int)(cv->lineWidth * cv_scale(cv) + 0.5f); if(lw < 1) lw = 1;
    /* Shadow pre-pass. */
    int sx = 0, sy = 0;
    if(cv_shadow_offsets(cv, &sx, &sy)) {
        uint32_t sc = color_scale_alpha(cv->shadowColor, cv->globalAlpha);
        if(sc != 0) {
            if(st->cb.stroke_text != NULL)
                st->cb.stroke_text(st->ctx, cv->canvas, px + sx, py + sy, text,
                                   cv->fontSize, lw, sc);
            else if(st->cb.draw_text != NULL)
                st->cb.draw_text(st->ctx, cv->canvas, px + sx, py + sy, text,
                                 cv->fontSize, sc);
        }
    }
    uint32_t c = color_scale_alpha(cv_style_color_at(&cv->strokeStyle, dx, dy), cv->globalAlpha);
    if(c == 0) return NULL;
    if(st->cb.stroke_text != NULL)
        st->cb.stroke_text(st->ctx, cv->canvas, px, py, text, cv->fontSize, lw, c);
    else if(st->cb.draw_text != NULL)
        st->cb.draw_text(st->ctx, cv->canvas, px, py, text, cv->fontSize, c);
    return NULL;
}

/* Build a TextMetrics object. The graph library only gives us advance width
 * and line height, so the bounding-box fields are approximated from those. */
static var_t* js_ctx_measureText(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    const char* text = get_str(env, "text");
    int tw = 0, th = 0;
    if(cv != NULL && st != NULL && st->cb.text_size != NULL && text != NULL)
        st->cb.text_size(st->ctx, cv->canvas, text, cv->fontSize, &tw, &th);
    var_t* obj = new_obj(vm, CLS_TEXTMETRICS, 0);
    if(obj == NULL) return var_new_null(vm);
    var_add(obj, "width", var_new_float(vm, (float)tw));
    var_add(obj, "actualBoundingBoxLeft", var_new_float(vm, 0.0f));
    var_add(obj, "actualBoundingBoxRight", var_new_float(vm, (float)tw));
    var_add(obj, "actualBoundingBoxAscent", var_new_float(vm, (float)(th * 0.8f)));
    var_add(obj, "actualBoundingBoxDescent", var_new_float(vm, (float)(th * 0.2f)));
    var_add(obj, "fontBoundingBoxAscent", var_new_float(vm, (float)(th * 0.8f)));
    var_add(obj, "fontBoundingBoxDescent", var_new_float(vm, (float)(th * 0.2f)));
    var_add(obj, "emHeightAscent", var_new_float(vm, (float)(th * 0.8f)));
    var_add(obj, "emHeightDescent", var_new_float(vm, (float)(th * 0.2f)));
    var_add(obj, "alphabeticBaseline", var_new_float(vm, 0.0f));
    return obj;
}

/* ------------------------------------------------------------------ */
/* Transform + state natives                                          */
/* ------------------------------------------------------------------ */

static var_t* js_ctx_save(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    cv->saves = (cv_save_t*)cv_grow(cv->saves, &cv->saves_cap, cv->saves_n + 1, sizeof(cv_save_t));
    if(cv->saves_n + 1 > cv->saves_cap) return NULL;
    cv_save_t* s = &cv->saves[cv->saves_n++];
    memset(s, 0, sizeof(*s));
    s->fillStyle = cv->fillStyle;
    s->strokeStyle = cv->strokeStyle;
    /* Bump refcounts so the saved style objects stay alive. */
    if(s->fillStyle.kind == 1 && s->fillStyle.obj) ((cv_gradient*)s->fillStyle.obj)->refcount++;
    if(s->fillStyle.kind == 2 && s->fillStyle.obj) ((cv_pattern*)s->fillStyle.obj)->refcount++;
    if(s->strokeStyle.kind == 1 && s->strokeStyle.obj) ((cv_gradient*)s->strokeStyle.obj)->refcount++;
    if(s->strokeStyle.kind == 2 && s->strokeStyle.obj) ((cv_pattern*)s->strokeStyle.obj)->refcount++;
    s->lineWidth = cv->lineWidth; s->fontSize = cv->fontSize;
    s->miterLimit = cv->miterLimit; s->lineDashOffset = cv->lineDashOffset;
    s->globalAlpha = cv->globalAlpha;
    s->textAlign = cv->textAlign; s->textBaseline = cv->textBaseline;
    s->lineCap = cv->lineCap; s->lineJoin = cv->lineJoin;
    s->globalCompositeOperation = cv->globalCompositeOperation;
    s->imageSmoothingEnabled = cv->imageSmoothingEnabled;
    s->imageSmoothingQuality = cv->imageSmoothingQuality;
    s->direction = cv->direction;
    s->letterSpacing = cv->letterSpacing; s->wordSpacing = cv->wordSpacing;
    s->fontKerning = cv->fontKerning; s->fontStretch = cv->fontStretch;
    s->fontVariantCaps = cv->fontVariantCaps; s->textRendering = cv->textRendering;
    s->shadowBlur = cv->shadowBlur;
    s->shadowOffsetX = cv->shadowOffsetX; s->shadowOffsetY = cv->shadowOffsetY;
    s->shadowColor = cv->shadowColor;
    s->a = cv->a; s->b = cv->b; s->c = cv->c; s->d = cv->d; s->e = cv->e; s->f = cv->f;
    s->clip_x = cv->clip_x; s->clip_y = cv->clip_y;
    s->clip_w = cv->clip_w; s->clip_h = cv->clip_h;
    s->has_clip = cv->has_clip;
    s->dash = NULL; s->dash_n = 0;
    if(cv->dash_n > 0) {
        s->dash = (float*)mario_malloc((uint32_t)(cv->dash_n * sizeof(float)));
        if(s->dash != NULL) {
            memcpy(s->dash, cv->dash, (size_t)cv->dash_n * sizeof(float));
            s->dash_n = cv->dash_n;
        }
    }
    s->fontFamily = cv_strdup(cv->fontFamily);
    return NULL;
}

static var_t* js_ctx_restore(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL || cv->saves_n == 0) return NULL;
    cv_save_t s = cv->saves[--cv->saves_n];
    /* Release the current styles before overwriting. */
    cv_style_release(cv->state, &cv->fillStyle);
    cv_style_release(cv->state, &cv->strokeStyle);
    cv->fillStyle = s.fillStyle;
    cv->strokeStyle = s.strokeStyle;
    cv->lineWidth = s.lineWidth; cv->fontSize = s.fontSize;
    cv->miterLimit = s.miterLimit; cv->lineDashOffset = s.lineDashOffset;
    cv->globalAlpha = s.globalAlpha;
    cv->textAlign = s.textAlign; cv->textBaseline = s.textBaseline;
    cv->lineCap = s.lineCap; cv->lineJoin = s.lineJoin;
    cv->globalCompositeOperation = s.globalCompositeOperation;
    cv->imageSmoothingEnabled = s.imageSmoothingEnabled;
    cv->imageSmoothingQuality = s.imageSmoothingQuality;
    cv->direction = s.direction;
    cv->letterSpacing = s.letterSpacing; cv->wordSpacing = s.wordSpacing;
    cv->fontKerning = s.fontKerning; cv->fontStretch = s.fontStretch;
    cv->fontVariantCaps = s.fontVariantCaps; cv->textRendering = s.textRendering;
    cv->shadowBlur = s.shadowBlur;
    cv->shadowOffsetX = s.shadowOffsetX; cv->shadowOffsetY = s.shadowOffsetY;
    cv->shadowColor = s.shadowColor;
    cv->a = s.a; cv->b = s.b; cv->c = s.c; cv->d = s.d; cv->e = s.e; cv->f = s.f;
    cv->clip_x = s.clip_x; cv->clip_y = s.clip_y;
    cv->clip_w = s.clip_w; cv->clip_h = s.clip_h;
    cv->has_clip = s.has_clip;
    /* Restore dash. */
    if(cv->dash != NULL) mario_free(cv->dash);
    cv->dash = s.dash; cv->dash_n = s.dash_n;
    cv->dash_cap = s.dash_n;
    /* Restore font family. */
    if(cv->fontFamily != NULL) mario_free(cv->fontFamily);
    cv->fontFamily = s.fontFamily;
    /* Push the clip back to the embedder. */
    if(cv->state != NULL) {
        if(cv->has_clip && cv->state->cb.set_clip != NULL)
            cv->state->cb.set_clip(cv->state->ctx, cv->canvas, cv->clip_x, cv->clip_y, cv->clip_w, cv->clip_h);
        else if(!cv->has_clip && cv->state->cb.clear_clip != NULL)
            cv->state->cb.clear_clip(cv->state->ctx, cv->canvas);
    }
    return NULL;
}

static var_t* js_ctx_reset(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    /* Reset to defaults per spec: clear path, reset transform, clear clip,
     * reset all styles. */
    cv->pts_n = 0; cv->subs_n = 0;
    cv_style_release(cv->state, &cv->fillStyle);
    cv_style_release(cv->state, &cv->strokeStyle);
    cv_style_set_color(&cv->fillStyle, 0xFF000000u);
    cv_style_set_color(&cv->strokeStyle, 0xFF000000u);
    cv->lineWidth = 1.0f; cv->miterLimit = 10.0f; cv->lineDashOffset = 0.0f;
    cv->globalAlpha = 1.0f;
    cv->lineCap = JS_LINE_CAP_BUTT; cv->lineJoin = JS_LINE_JOIN_MITER;
    cv->globalCompositeOperation = JS_GCO_SOURCE_OVER;
    cv->imageSmoothingEnabled = 1; cv->imageSmoothingQuality = JS_SMOOTH_LOW;
    cv->shadowBlur = 0; cv->shadowOffsetX = 0; cv->shadowOffsetY = 0;
    cv->shadowColor = 0;
    cv->fontSize = 10.0f; cv->textAlign = JS_TEXT_ALIGN_START;
    cv->textBaseline = JS_TEXT_BASELINE_ALPHABETIC;
    cv->a = 1; cv->b = 0; cv->c = 0; cv->d = 1; cv->e = 0; cv->f = 0;
    cv->has_clip = 0;
    if(cv->dash != NULL) { mario_free(cv->dash); cv->dash = NULL; cv->dash_n = cv->dash_cap = 0; }
    if(cv->state != NULL && cv->state->cb.clear_clip != NULL)
        cv->state->cb.clear_clip(cv->state->ctx, cv->canvas);
    return NULL;
}

static var_t* js_ctx_translate(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float tx = get_float(env, "x"), ty = get_float(env, "y");
    cv->e += cv->a * tx + cv->c * ty;
    cv->f += cv->b * tx + cv->d * ty;
    return NULL;
}

static var_t* js_ctx_rotate(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float ang = get_float(env, "a");
    float co = cosf(ang), si = sinf(ang);
    float a = cv->a, b = cv->b, c = cv->c, d = cv->d;
    cv->a = a * co + c * si;
    cv->b = b * co + d * si;
    cv->c = c * co - a * si;
    cv->d = d * co - b * si;
    return NULL;
}

static var_t* js_ctx_scale(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float sx = get_float(env, "x"), sy = get_float(env, "y");
    cv->a *= sx; cv->b *= sx; cv->c *= sy; cv->d *= sy;
    return NULL;
}

static var_t* js_ctx_transform(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    float a = get_float(env, "a"), b = get_float(env, "b");
    float c = get_float(env, "c"), d = get_float(env, "d");
    float e = get_float(env, "e"), f = get_float(env, "f");
    /* CTM = CTM * M */
    float na = cv->a * a + cv->c * b;
    float nb = cv->b * a + cv->d * b;
    float nc = cv->a * c + cv->c * d;
    float nd = cv->b * c + cv->d * d;
    float ne = cv->a * e + cv->c * f + cv->e;
    float nf = cv->b * e + cv->d * f + cv->f;
    cv->a = na; cv->b = nb; cv->c = nc; cv->d = nd; cv->e = ne; cv->f = nf;
    return NULL;
}

static var_t* js_ctx_setTransform(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    /* Accept either 6 scalars or a DOMMatrix-like object with a/b/c/d/e/f. */
    var_t* args = get_func_args(env);
    node_t* n0 = var_array_get(args, 0);
    if(n0 != NULL && n0->var != NULL && var_get_str(n0->var) == NULL) {
        /* Might be an object; try reading members. */
        var_t* o = n0->var;
        var_t* na = var_find_own_member_var(o, "a");
        if(na != NULL) {
            cv->a = var_get_float(na);
            cv->b = var_get_float(var_find_own_member_var(o, "b"));
            cv->c = var_get_float(var_find_own_member_var(o, "c"));
            cv->d = var_get_float(var_find_own_member_var(o, "d"));
            cv->e = var_get_float(var_find_own_member_var(o, "e"));
            cv->f = var_get_float(var_find_own_member_var(o, "f"));
            return NULL;
        }
    }
    cv->a = get_float(env, "a"); cv->b = get_float(env, "b");
    cv->c = get_float(env, "c"); cv->d = get_float(env, "d");
    cv->e = get_float(env, "e"); cv->f = get_float(env, "f");
    return NULL;
}

static var_t* js_ctx_resetTransform(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    cv->a = 1; cv->b = 0; cv->c = 0; cv->d = 1; cv->e = 0; cv->f = 0;
    return NULL;
}

static var_t* js_ctx_getTransform(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_canvas_ctx* cv = ctx_from_env(env);
    var_t* obj = new_obj(vm, CLS_DOMMATRIX, 0);
    if(obj == NULL) return var_new_null(vm);
    if(cv == NULL) return obj;
    var_add(obj, "a", var_new_float(vm, cv->a));
    var_add(obj, "b", var_new_float(vm, cv->b));
    var_add(obj, "c", var_new_float(vm, cv->c));
    var_add(obj, "d", var_new_float(vm, cv->d));
    var_add(obj, "e", var_new_float(vm, cv->e));
    var_add(obj, "f", var_new_float(vm, cv->f));
    var_add(obj, "m11", var_new_float(vm, cv->a));
    var_add(obj, "m12", var_new_float(vm, cv->b));
    var_add(obj, "m21", var_new_float(vm, cv->c));
    var_add(obj, "m22", var_new_float(vm, cv->d));
    var_add(obj, "m41", var_new_float(vm, cv->e));
    var_add(obj, "m42", var_new_float(vm, cv->f));
    var_add(obj, "is2D", var_new_bool(vm, true));
    var_add(obj, "isIdentity", var_new_bool(vm,
            cv->a == 1 && cv->b == 0 && cv->c == 0 && cv->d == 1 && cv->e == 0 && cv->f == 0));
    return obj;
}


/* ------------------------------------------------------------------ */
/* Style accessors                                                    */
/* ------------------------------------------------------------------ */

/* Format an ARGB color back to a CSS string. Used by the fillStyle/
 * strokeStyle/shadowColor getters. */
static void color_to_css(uint32_t c, char* buf, size_t n) {
    int a = (int)((c >> 24) & 0xFF);
    int r = (int)((c >> 16) & 0xFF);
    int g = (int)((c >> 8) & 0xFF);
    int b = (int)(c & 0xFF);
    if(a == 255) snprintf(buf, n, "#%02X%02X%02X", r, g, b);
    else snprintf(buf, n, "rgba(%d,%d,%d,%.3f)", r, g, b, (float)a / 255.0f);
}

static var_t* js_ctx_get_fillStyle(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL) return var_new_str(vm, "#000000");
    if(cv->fillStyle.kind == 0) {
        char buf[40]; color_to_css(cv->fillStyle.color, buf, sizeof(buf));
        return var_new_str(vm, buf);
    }
    /* Return the gradient/pattern object. The JS instance was stashed in the
     * style's obj->js_instance field when assigned; if absent, return a fresh
     * wrapper (rare, only for internally-created styles). */
    return var_new_str(vm, "");
}

static var_t* js_ctx_set_fillStyle(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    /* If the value is a CanvasGradient/CanvasPattern instance, adopt it. */
    var_t* v = n->var;
    if(v->value != NULL) {
        /* Heuristic: a class instance with a value pointer is a gradient or
         * pattern. Check the class name via the prototype chain. */
        var_t* proto = var_get_prototype(v);
        if(proto != NULL) {
            const char* cname = var_get_str(var_find_own_member_var(proto, "__class_name"));
            if(cname == NULL) cname = "";
            if(!strcmp(cname, CLS_GRADIENT)) {
                cv_style_set_obj(cv->state, &cv->fillStyle, 1, v->value);
                return NULL;
            }
            if(!strcmp(cname, CLS_PATTERN)) {
                cv_style_set_obj(cv->state, &cv->fillStyle, 2, v->value);
                return NULL;
            }
        }
    }
    const char* s = var_get_str(v);
    if(s == NULL) return NULL;
    cv_style_set_color(&cv->fillStyle, parse_color(s));
    return NULL;
}

static var_t* js_ctx_get_strokeStyle(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL) return var_new_str(vm, "#000000");
    if(cv->strokeStyle.kind == 0) {
        char buf[40]; color_to_css(cv->strokeStyle.color, buf, sizeof(buf));
        return var_new_str(vm, buf);
    }
    return var_new_str(vm, "");
}

static var_t* js_ctx_set_strokeStyle(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    var_t* v = n->var;
    if(v->value != NULL) {
        var_t* proto = var_get_prototype(v);
        if(proto != NULL) {
            const char* cname = var_get_str(var_find_own_member_var(proto, "__class_name"));
            if(cname == NULL) cname = "";
            if(!strcmp(cname, CLS_GRADIENT)) {
                cv_style_set_obj(cv->state, &cv->strokeStyle, 1, v->value);
                return NULL;
            }
            if(!strcmp(cname, CLS_PATTERN)) {
                cv_style_set_obj(cv->state, &cv->strokeStyle, 2, v->value);
                return NULL;
            }
        }
    }
    const char* s = var_get_str(v);
    if(s == NULL) return NULL;
    cv_style_set_color(&cv->strokeStyle, parse_color(s));
    return NULL;
}

static var_t* js_ctx_get_lineWidth(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_float(vm, cv ? cv->lineWidth : 1.0f);
}
static var_t* js_ctx_set_lineWidth(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv != NULL && n != NULL && n->var != NULL) {
        float w = var_get_float(n->var);
        if(w > 0.0f && isfinite(w)) cv->lineWidth = w;
    }
    return NULL;
}

static var_t* js_ctx_get_lineCap(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    const char* s = "butt";
    if(cv != NULL) {
        if(cv->lineCap == JS_LINE_CAP_ROUND) s = "round";
        else if(cv->lineCap == JS_LINE_CAP_SQUARE) s = "square";
    }
    return var_new_str(vm, s);
}
static var_t* js_ctx_set_lineCap(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var); if(s == NULL) return NULL;
    if(!strcasecmp(s, "round")) cv->lineCap = JS_LINE_CAP_ROUND;
    else if(!strcasecmp(s, "square")) cv->lineCap = JS_LINE_CAP_SQUARE;
    else cv->lineCap = JS_LINE_CAP_BUTT;
    return NULL;
}

static var_t* js_ctx_get_lineJoin(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    const char* s = "miter";
    if(cv != NULL) {
        if(cv->lineJoin == JS_LINE_JOIN_ROUND) s = "round";
        else if(cv->lineJoin == JS_LINE_JOIN_BEVEL) s = "bevel";
    }
    return var_new_str(vm, s);
}
static var_t* js_ctx_set_lineJoin(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var); if(s == NULL) return NULL;
    if(!strcasecmp(s, "round")) cv->lineJoin = JS_LINE_JOIN_ROUND;
    else if(!strcasecmp(s, "bevel")) cv->lineJoin = JS_LINE_JOIN_BEVEL;
    else cv->lineJoin = JS_LINE_JOIN_MITER;
    return NULL;
}

static var_t* js_ctx_get_miterLimit(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_float(vm, cv ? cv->miterLimit : 10.0f);
}
static var_t* js_ctx_set_miterLimit(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv != NULL && n != NULL && n->var != NULL) {
        float v = var_get_float(n->var);
        if(v > 0.0f && isfinite(v)) cv->miterLimit = v;
    }
    return NULL;
}

static var_t* js_ctx_get_lineDashOffset(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_float(vm, cv ? cv->lineDashOffset : 0.0f);
}
static var_t* js_ctx_set_lineDashOffset(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv != NULL && n != NULL && n->var != NULL) {
        float v = var_get_float(n->var);
        if(isfinite(v)) cv->lineDashOffset = v;
    }
    return NULL;
}

static var_t* js_ctx_setLineDash(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    var_t* args = get_func_args(env);
    node_t* n0 = var_array_get(args, 0);
    if(n0 == NULL || n0->var == NULL) return NULL;
    var_t* arr = n0->var;
    uint32_t len = var_array_size(arr);
    /* Validate: all finite non-negative. */
    float tmp[64];
    int cnt = 0;
    for(uint32_t i = 0; i < len && cnt < 64; ++i) {
        node_t* ni = var_array_get(arr, (int32_t)i);
        if(ni == NULL || ni->var == NULL) continue;
        float v = var_get_float(ni->var);
        if(!isfinite(v) || v < 0) return NULL;
        tmp[cnt++] = v;
    }
    /* Per spec, odd-length arrays are doubled. */
    int total = cnt;
    if(cnt % 2 == 1) total = cnt * 2;
    if(total == 0) {
        if(cv->dash != NULL) { mario_free(cv->dash); cv->dash = NULL; }
        cv->dash_n = cv->dash_cap = 0;
        return NULL;
    }
    /* Scale dash lengths by the CTM so they track the transform. */
    float sc = cv_scale(cv);
    if(sc <= 0 || !isfinite(sc)) sc = 1.0f;
    cv->dash = (float*)cv_grow(cv->dash, &cv->dash_cap, total, sizeof(float));
    if(cv->dash_cap < total) return NULL;
    for(int i = 0; i < total; ++i) cv->dash[i] = tmp[i % cnt] * sc;
    cv->dash_n = total;
    return NULL;
}

static var_t* js_ctx_getLineDash(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_canvas_ctx* cv = ctx_from_env(env);
    var_t* arr = var_new_array(vm);
    if(arr == NULL || cv == NULL) return arr;
    /* Un-scale by the CTM so JS sees the user-space values it set. */
    float sc = cv_scale(cv);
    if(sc <= 0 || !isfinite(sc)) sc = 1.0f;
    for(int i = 0; i < cv->dash_n; ++i)
        var_array_add(arr, var_new_float(vm, cv->dash[i] / sc));
    return arr;
}

static var_t* js_ctx_get_globalAlpha(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_float(vm, cv ? cv->globalAlpha : 1.0f);
}
static var_t* js_ctx_set_globalAlpha(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv != NULL && n != NULL && n->var != NULL) {
        float v = var_get_float(n->var);
        if(isfinite(v) && v >= 0.0f && v <= 1.0f) cv->globalAlpha = v;
    }
    return NULL;
}

static const char* GCO_NAMES[] = {
    "source-over","source-in","source-out","source-atop",
    "destination-over","destination-in","destination-out","destination-atop",
    "lighter","copy","xor","multiply","screen","overlay","darken","lighten",
    "color-dodge","color-burn","hard-light","soft-light","difference",
    "exclusion","hue","saturation","color","luminosity"
};
#define GCO_NAMES_N ((int)(sizeof(GCO_NAMES)/sizeof(GCO_NAMES[0])))

static var_t* js_ctx_get_globalCompositeOperation(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    int i = (cv != NULL) ? cv->globalCompositeOperation : 0;
    if(i < 0 || i >= GCO_NAMES_N) i = 0;
    return var_new_str(vm, GCO_NAMES[i]);
}
static var_t* js_ctx_set_globalCompositeOperation(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var); if(s == NULL) return NULL;
    for(int i = 0; i < GCO_NAMES_N; ++i) {
        if(!strcasecmp(s, GCO_NAMES[i])) { cv->globalCompositeOperation = i; return NULL; }
    }
    /* Unknown value: per spec, ignore. */
    return NULL;
}

static var_t* js_ctx_get_imageSmoothingEnabled(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_bool(vm, cv ? (cv->imageSmoothingEnabled != 0) : true);
}
static var_t* js_ctx_set_imageSmoothingEnabled(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var);
    if(s != NULL) cv->imageSmoothingEnabled = (!strcasecmp(s, "true") || !strcmp(s, "1")) ? 1 : 0;
    else cv->imageSmoothingEnabled = var_get_int(n->var) ? 1 : 0;
    return NULL;
}

static var_t* js_ctx_get_imageSmoothingQuality(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    const char* s = "low";
    if(cv != NULL) {
        if(cv->imageSmoothingQuality == JS_SMOOTH_MEDIUM) s = "medium";
        else if(cv->imageSmoothingQuality == JS_SMOOTH_HIGH) s = "high";
    }
    return var_new_str(vm, s);
}
static var_t* js_ctx_set_imageSmoothingQuality(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var); if(s == NULL) return NULL;
    if(!strcasecmp(s, "medium")) cv->imageSmoothingQuality = JS_SMOOTH_MEDIUM;
    else if(!strcasecmp(s, "high")) cv->imageSmoothingQuality = JS_SMOOTH_HIGH;
    else cv->imageSmoothingQuality = JS_SMOOTH_LOW;
    return NULL;
}

static var_t* js_ctx_get_shadowBlur(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_float(vm, cv ? cv->shadowBlur : 0.0f);
}
static var_t* js_ctx_set_shadowBlur(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv != NULL && n != NULL && n->var != NULL) {
        float v = var_get_float(n->var);
        if(isfinite(v) && v >= 0.0f) cv->shadowBlur = v;
    }
    return NULL;
}

static var_t* js_ctx_get_shadowColor(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    char buf[40]; color_to_css(cv ? cv->shadowColor : 0u, buf, sizeof(buf));
    return var_new_str(vm, buf);
}
static var_t* js_ctx_set_shadowColor(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv != NULL && n != NULL && n->var != NULL)
        cv->shadowColor = parse_color(var_get_str(n->var));
    return NULL;
}

static var_t* js_ctx_get_shadowOffsetX(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_float(vm, cv ? cv->shadowOffsetX : 0.0f);
}
static var_t* js_ctx_set_shadowOffsetX(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv != NULL && n != NULL && n->var != NULL) {
        float v = var_get_float(n->var);
        if(isfinite(v)) cv->shadowOffsetX = v;
    }
    return NULL;
}
static var_t* js_ctx_get_shadowOffsetY(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_float(vm, cv ? cv->shadowOffsetY : 0.0f);
}
static var_t* js_ctx_set_shadowOffsetY(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv != NULL && n != NULL && n->var != NULL) {
        float v = var_get_float(n->var);
        if(isfinite(v)) cv->shadowOffsetY = v;
    }
    return NULL;
}

static var_t* js_ctx_get_font(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    char buf[128];
    if(cv == NULL) return var_new_str(vm, "10px sans-serif");
    const char* style = cv->fontItalic ? "italic " : "";
    const char* weight = "normal";
    char wbuf[8] = "";
    if(cv->fontWeight == 700) weight = "bold";
    else if(cv->fontWeight != 400) { snprintf(wbuf, sizeof(wbuf), "%d", cv->fontWeight); weight = wbuf; }
    const char* fam = (cv->fontFamily != NULL) ? cv->fontFamily : "sans-serif";
    snprintf(buf, sizeof(buf), "%s%s %dpx %s", style, weight, (int)(cv->fontSize + 0.5f), fam);
    return var_new_str(vm, buf);
}
static var_t* js_ctx_set_font(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var);
    if(s == NULL) return NULL;
    float sz = cv->fontSize;
    char* fam = NULL;
    int wt = cv->fontWeight, it = cv->fontItalic;
    parse_font_shorthand(s, &sz, &fam, &wt, &it);
    if(sz > 0.0f && isfinite(sz)) cv->fontSize = sz;
    if(fam != NULL) {
        if(cv->fontFamily != NULL) mario_free(cv->fontFamily);
        cv->fontFamily = fam;
    }
    cv->fontWeight = wt;
    cv->fontItalic = it;
    return NULL;
}

static var_t* js_ctx_get_textAlign(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    const char* s = "start";
    if(cv != NULL) {
        switch(cv->textAlign) {
            case JS_TEXT_ALIGN_LEFT: s = "left"; break;
            case JS_TEXT_ALIGN_CENTER: s = "center"; break;
            case JS_TEXT_ALIGN_RIGHT: s = "right"; break;
            case JS_TEXT_ALIGN_END: s = "end"; break;
            default: s = "start";
        }
    }
    return var_new_str(vm, s);
}
static var_t* js_ctx_set_textAlign(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var); if(s == NULL) return NULL;
    if(!strcasecmp(s, "left")) cv->textAlign = JS_TEXT_ALIGN_LEFT;
    else if(!strcasecmp(s, "center")) cv->textAlign = JS_TEXT_ALIGN_CENTER;
    else if(!strcasecmp(s, "right")) cv->textAlign = JS_TEXT_ALIGN_RIGHT;
    else if(!strcasecmp(s, "end")) cv->textAlign = JS_TEXT_ALIGN_END;
    else cv->textAlign = JS_TEXT_ALIGN_START;
    return NULL;
}

static var_t* js_ctx_get_textBaseline(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    const char* s = "alphabetic";
    if(cv != NULL) {
        switch(cv->textBaseline) {
            case JS_TEXT_BASELINE_TOP: s = "top"; break;
            case JS_TEXT_BASELINE_HANGING: s = "hanging"; break;
            case JS_TEXT_BASELINE_MIDDLE: s = "middle"; break;
            case JS_TEXT_BASELINE_IDEOGRAPHIC: s = "ideographic"; break;
            case JS_TEXT_BASELINE_BOTTOM: s = "bottom"; break;
            default: s = "alphabetic";
        }
    }
    return var_new_str(vm, s);
}
static var_t* js_ctx_set_textBaseline(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var); if(s == NULL) return NULL;
    if(!strcasecmp(s, "top")) cv->textBaseline = JS_TEXT_BASELINE_TOP;
    else if(!strcasecmp(s, "hanging")) cv->textBaseline = JS_TEXT_BASELINE_HANGING;
    else if(!strcasecmp(s, "middle")) cv->textBaseline = JS_TEXT_BASELINE_MIDDLE;
    else if(!strcasecmp(s, "ideographic")) cv->textBaseline = JS_TEXT_BASELINE_IDEOGRAPHIC;
    else if(!strcasecmp(s, "bottom")) cv->textBaseline = JS_TEXT_BASELINE_BOTTOM;
    else cv->textBaseline = JS_TEXT_BASELINE_ALPHABETIC;
    return NULL;
}

static var_t* js_ctx_get_direction(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    const char* s = "inherit";
    if(cv != NULL) {
        if(cv->direction == 0) s = "ltr";
        else if(cv->direction == 1) s = "rtl";
    }
    return var_new_str(vm, s);
}
static var_t* js_ctx_set_direction(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var); if(s == NULL) return NULL;
    if(!strcasecmp(s, "ltr")) cv->direction = 0;
    else if(!strcasecmp(s, "rtl")) cv->direction = 1;
    else cv->direction = 2;
    return NULL;
}

static var_t* js_ctx_get_letterSpacing(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    char buf[24]; snprintf(buf, sizeof(buf), "%gpx", cv ? cv->letterSpacing : 0.0f);
    return var_new_str(vm, buf);
}
static var_t* js_ctx_set_letterSpacing(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var);
    cv->letterSpacing = (s != NULL) ? cv_atof(s) : var_get_float(n->var);
    return NULL;
}
static var_t* js_ctx_get_wordSpacing(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    char buf[24]; snprintf(buf, sizeof(buf), "%gpx", cv ? cv->wordSpacing : 0.0f);
    return var_new_str(vm, buf);
}
static var_t* js_ctx_set_wordSpacing(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var);
    cv->wordSpacing = (s != NULL) ? cv_atof(s) : var_get_float(n->var);
    return NULL;
}

static var_t* js_ctx_get_fontKerning(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    const char* s = "auto";
    if(cv != NULL && cv->fontKerning == 1) s = "normal";
    else if(cv != NULL && cv->fontKerning == 2) s = "none";
    return var_new_str(vm, s);
}
static var_t* js_ctx_set_fontKerning(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var); if(s == NULL) return NULL;
    if(!strcasecmp(s, "normal")) cv->fontKerning = 1;
    else if(!strcasecmp(s, "none")) cv->fontKerning = 2;
    else cv->fontKerning = 0;
    return NULL;
}

static var_t* js_ctx_get_fontStretch(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    (void)cv;
    return var_new_str(vm, "normal");
}
static var_t* js_ctx_set_fontStretch(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    /* Accepted and ignored; the embedder's font subsystem has one stretch. */
    cv->fontStretch = 0;
    return NULL;
}

static var_t* js_ctx_get_fontVariantCaps(vm_t* vm, var_t* env, void* data) {
    (void)data; (void)env;
    return var_new_str(vm, "normal");
}
static var_t* js_ctx_set_fontVariantCaps(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    cv->fontVariantCaps = 0;
    return NULL;
}

static var_t* js_ctx_get_textRendering(vm_t* vm, var_t* env, void* data) {
    (void)data; (void)env;
    return var_new_str(vm, "auto");
}
static var_t* js_ctx_set_textRendering(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    cv->textRendering = 0;
    return NULL;
}

static var_t* js_ctx_get_filter(vm_t* vm, var_t* env, void* data) {
    (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    return var_new_str(vm, (cv != NULL && cv->filter != NULL) ? cv->filter : "none");
}
static var_t* js_ctx_set_filter(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; js_canvas_ctx* cv = ctx_from_env(env);
    node_t* n = setter_arg(env);
    if(cv == NULL || n == NULL || n->var == NULL) return NULL;
    const char* s = var_get_str(n->var);
    if(cv->filter != NULL) { mario_free(cv->filter); cv->filter = NULL; }
    if(s != NULL) cv->filter = cv_strdup(s);
    return NULL;
}

/* canvas: read-only property returning the backing HTMLCanvasElement. The
 * bridge does not own the element handle, so we return a thin Element-class
 * wrapper whose ->value is the canvas id string (enough for JS to read
 * width/height back through the Element accessors). */
static var_t* js_ctx_get_canvas(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL) return var_new_null(vm);
    var_t* obj = new_obj(vm, CLS_ELEMENT, 0);
    if(obj == NULL) return var_new_null(vm);
    /* Stash the canvas handle so Element.width/height (registered by this
     * bridge) can read the dims back through canvas_dims. */
    obj->value = cv->canvas;
    obj->free_func = NULL;
    return obj;
}

static var_t* js_ctx_getContextAttributes(vm_t* vm, var_t* env, void* data) {
    (void)data;
    js_canvas_ctx* cv = ctx_from_env(env);
    var_t* obj = var_new_obj_no_proto(vm, NULL, NULL);
    if(obj == NULL) return var_new_null(vm);
    var_add(obj, "alpha", var_new_bool(vm, true));
    var_add(obj, "desynchronized", var_new_bool(vm, false));
    var_add(obj, "colorSpace", var_new_str(vm, "srgb"));
    var_add(obj, "willReadFrequently", var_new_bool(vm, false));
    (void)cv;
    return obj;
}


/* ------------------------------------------------------------------ */
/* CanvasGradient                                                     */
/* ------------------------------------------------------------------ */

static void js_grad_free(void* p) {
    cv_gradient* g = (cv_gradient*)p;
    if(g == NULL) return;
    g->refcount--;
    if(g->refcount <= 0) cv_grad_free(g);
}

static var_t* js_grad_addColorStop(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* t = get_obj(env, THIS);
    if(t == NULL || t->value == NULL) return NULL;
    cv_gradient* g = (cv_gradient*)t->value;
    float offset = get_float(env, "offset");
    const char* color = get_str(env, "color");
    if(!isfinite(offset) || offset < 0.0f || offset > 1.0f) {
        /* Per spec, throw IndexSizeError. mario has no exception plumbing
         * here, so we silently ignore. */
        return NULL;
    }
    if(color == NULL) return NULL;
    uint32_t c = parse_color(color);
    /* Insert sorted by offset (stable for equal offsets: newer goes after). */
    int pos = g->stops_n;
    for(int i = 0; i < g->stops_n; ++i) {
        if(g->stops[i].offset > offset) { pos = i; break; }
    }
    g->stops = (cv_grad_stop_t*)cv_grow(g->stops, &g->stops_cap, g->stops_n + 1, sizeof(cv_grad_stop_t));
    if(g->stops_cap < g->stops_n + 1) return NULL;
    for(int i = g->stops_n; i > pos; --i) g->stops[i] = g->stops[i-1];
    g->stops[pos].offset = offset;
    g->stops[pos].color = c;
    g->stops_n++;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CanvasPattern                                                      */
/* ------------------------------------------------------------------ */

static void js_pat_free(void* p) {
    cv_pattern* pat = (cv_pattern*)p;
    if(pat == NULL) return;
    pat->refcount--;
    if(pat->refcount <= 0) cv_pat_free(pat);
}

static var_t* js_pat_setTransform(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* t = get_obj(env, THIS);
    if(t == NULL || t->value == NULL) return NULL;
    cv_pattern* pat = (cv_pattern*)t->value;
    var_t* args = get_func_args(env);
    node_t* n0 = var_array_get(args, 0);
    if(n0 == NULL || n0->var == NULL) return NULL;
    var_t* m = n0->var;
    var_t* na = var_find_own_member_var(m, "a");
    if(na == NULL) return NULL;
    pat->a = var_get_float(na);
    pat->b = var_get_float(var_find_own_member_var(m, "b"));
    pat->c = var_get_float(var_find_own_member_var(m, "c"));
    pat->d = var_get_float(var_find_own_member_var(m, "d"));
    pat->e = var_get_float(var_find_own_member_var(m, "e"));
    pat->f = var_get_float(var_find_own_member_var(m, "f"));
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Gradient / pattern factories                                       */
/* ------------------------------------------------------------------ */

static cv_gradient* register_gradient(js_canvas_state* st, cv_gradient* g) {
    if(st == NULL || g == NULL) return g;
    st->grads = (cv_gradient**)cv_grow(st->grads, &st->grads_cap, st->grads_n + 1, sizeof(cv_gradient*));
    if(st->grads_cap < st->grads_n + 1) { cv_grad_free(g); return NULL; }
    st->grads[st->grads_n++] = g;
    return g;
}

static cv_pattern* register_pattern(js_canvas_state* st, cv_pattern* p) {
    if(st == NULL || p == NULL) return p;
    st->pats = (cv_pattern**)cv_grow(st->pats, &st->pats_cap, st->pats_n + 1, sizeof(cv_pattern*));
    if(st->pats_cap < st->pats_n + 1) { cv_pat_free(p); return NULL; }
    st->pats[st->pats_n++] = p;
    return p;
}

static var_t* js_ctx_createLinearGradient(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL) return var_new_null(vm);
    float x0 = get_float(env, "x0"), y0 = get_float(env, "y0");
    float x1 = get_float(env, "x1"), y1 = get_float(env, "y1");
    if(!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1))
        return var_new_null(vm);
    cv_gradient* g = cv_grad_new(JS_GRAD_LINEAR);
    if(g == NULL) return var_new_null(vm);
    g->x0 = x0; g->y0 = y0; g->x1 = x1; g->y1 = y1;
    if(register_gradient(st, g) == NULL) return var_new_null(vm);
    var_t* obj = new_obj(vm, CLS_GRADIENT, 0);
    if(obj == NULL) return var_new_null(vm);
    obj->value = (void*)g;
    obj->free_func = js_grad_free;
    g->refcount++;   /* one for the JS instance */
    return obj;
}

static var_t* js_ctx_createRadialGradient(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL) return var_new_null(vm);
    float x0 = get_float(env, "x0"), y0 = get_float(env, "y0"), r0 = get_float(env, "r0");
    float x1 = get_float(env, "x1"), y1 = get_float(env, "y1"), r1 = get_float(env, "r1");
    if(r0 < 0 || r1 < 0 || !isfinite(r0) || !isfinite(r1)) return var_new_null(vm);
    cv_gradient* g = cv_grad_new(JS_GRAD_RADIAL);
    if(g == NULL) return var_new_null(vm);
    g->x0 = x0; g->y0 = y0; g->r0 = r0;
    g->x1 = x1; g->y1 = y1; g->r1 = r1;
    if(register_gradient(st, g) == NULL) return var_new_null(vm);
    var_t* obj = new_obj(vm, CLS_GRADIENT, 0);
    if(obj == NULL) return var_new_null(vm);
    obj->value = (void*)g;
    obj->free_func = js_grad_free;
    g->refcount++;
    return obj;
}

static var_t* js_ctx_createConicGradient(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL) return var_new_null(vm);
    float startAngle = get_float(env, "startAngle");
    float x = get_float(env, "x"), y = get_float(env, "y");
    if(!isfinite(startAngle) || !isfinite(x) || !isfinite(y)) return var_new_null(vm);
    cv_gradient* g = cv_grad_new(JS_GRAD_CONIC);
    if(g == NULL) return var_new_null(vm);
    g->angle = startAngle; g->cx = x; g->cy = y;
    if(register_gradient(st, g) == NULL) return var_new_null(vm);
    var_t* obj = new_obj(vm, CLS_GRADIENT, 0);
    if(obj == NULL) return var_new_null(vm);
    obj->value = (void*)g;
    obj->free_func = js_grad_free;
    g->refcount++;
    return obj;
}

static var_t* js_ctx_createPattern(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || st == NULL) return var_new_null(vm);
    var_t* args = get_func_args(env);
    node_t* nimg = var_array_get(args, 0);
    node_t* nrep = var_array_get(args, 1);
    if(nimg == NULL || nimg->var == NULL) return var_new_null(vm);
    var_t* img = nimg->var;
    /* Resolve the image source: an Element (<img> or <canvas>) or an
     * ImageData instance. */
    void* bitmap = NULL;
    if(img->value != NULL) {
        var_t* proto = var_get_prototype(img);
        const char* cname = "";
        if(proto != NULL) {
            const char* s = var_get_str(var_find_own_member_var(proto, "__class_name"));
            if(s != NULL) cname = s;
        }
        if(!strcmp(cname, CLS_IMAGEDATA)) {
            /* ImageData carries a bitmap handle in ->value. */
            bitmap = img->value;
        } else if(!strcmp(cname, CLS_ELEMENT)) {
            /* Could be <img> or <canvas>. Try bitmap_from_element first; if
             * that fails, treat the value as a canvas handle. */
            if(st->cb.bitmap_from_element != NULL)
                bitmap = st->cb.bitmap_from_element(st->ctx, img->value);
            if(bitmap == NULL) bitmap = img->value;   /* canvas handle */
        } else {
            bitmap = img->value;
        }
    }
    if(bitmap == NULL) return var_new_null(vm);
    int rep = JS_PATTERN_REPEAT;
    if(nrep != NULL && nrep->var != NULL) {
        const char* s = var_get_str(nrep->var);
        if(s != NULL) {
            if(!strcasecmp(s, "repeat-x")) rep = JS_PATTERN_REPEAT_X;
            else if(!strcasecmp(s, "repeat-y")) rep = JS_PATTERN_REPEAT_Y;
            else if(!strcasecmp(s, "no-repeat")) rep = JS_PATTERN_NO_REPEAT;
            else rep = JS_PATTERN_REPEAT;
        }
    }
    cv_pattern* p = cv_pat_new();
    if(p == NULL) return var_new_null(vm);
    p->image = bitmap;
    p->repeat = rep;
    if(register_pattern(st, p) == NULL) return var_new_null(vm);
    var_t* obj = new_obj(vm, CLS_PATTERN, 0);
    if(obj == NULL) return var_new_null(vm);
    obj->value = (void*)p;
    obj->free_func = js_pat_free;
    p->refcount++;
    return obj;
}

/* ------------------------------------------------------------------ */
/* drawImage                                                          */
/* ------------------------------------------------------------------ */

/* Resolve a JS image argument to an opaque bitmap handle plus its natural
 * dimensions. Returns 0 on success. */
static int cv_resolve_image(js_canvas_state* st, var_t* img, void** out_bmp, int* out_w, int* out_h) {
    *out_bmp = NULL; *out_w = 0; *out_h = 0;
    if(img == NULL || st == NULL) return -1;
    void* bmp = NULL;
    if(img->value != NULL) {
        var_t* proto = var_get_prototype(img);
        const char* cname = "";
        if(proto != NULL) {
            const char* s = var_get_str(var_find_own_member_var(proto, "__class_name"));
            if(s != NULL) cname = s;
        }
        if(!strcmp(cname, CLS_IMAGEDATA)) {
            bmp = img->value;
        } else if(!strcmp(cname, CLS_ELEMENT)) {
            if(st->cb.bitmap_from_element != NULL)
                bmp = st->cb.bitmap_from_element(st->ctx, img->value);
            if(bmp == NULL) bmp = img->value;
        } else if(!strcmp(cname, CLS_CTX2D)) {
            /* drawImage(otherCtx) is not in the spec but some code does it;
             * treat as the ctx's canvas. */
            js_canvas_ctx* oc = (js_canvas_ctx*)img->value;
            bmp = oc->canvas;
        } else {
            bmp = img->value;
        }
    }
    if(bmp == NULL) return -1;
    int w = 0, h = 0;
    if(st->cb.bitmap_dims != NULL) st->cb.bitmap_dims(st->ctx, bmp, &w, &h);
    if(w <= 0 || h <= 0) {
        /* Maybe it is a canvas handle; try canvas_dims. */
        if(st->cb.canvas_dims != NULL) st->cb.canvas_dims(st->ctx, bmp, &w, &h);
    }
    if(w <= 0 || h <= 0) return -1;
    *out_bmp = bmp; *out_w = w; *out_h = h;
    return 0;
}

static var_t* js_ctx_drawImage(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || cv->canvas == NULL || st == NULL || st->cb.blit == NULL) return NULL;
    var_t* args = get_func_args(env);
    uint32_t argc = var_array_size(args);
    if(argc < 3) return NULL;
    node_t* nimg = var_array_get(args, 0);
    if(nimg == NULL || nimg->var == NULL) return NULL;
    void* bmp; int bw, bh;
    if(cv_resolve_image(st, nimg->var, &bmp, &bw, &bh) != 0) return NULL;

    float v[8];
    for(int i = 0; i < 8; ++i) v[i] = 0.0f;
    for(uint32_t i = 1; i < argc && i <= 8; ++i) {
        node_t* ni = var_array_get(args, (int32_t)i);
        if(ni != NULL && ni->var != NULL) v[i-1] = var_get_float(ni->var);
    }
    float sx, sy, sw, sh, dx, dy, dw, dh;
    if(argc == 3) {
        sx = 0; sy = 0; sw = bw; sh = bh;
        dx = v[0]; dy = v[1]; dw = bw; dh = bh;
    } else if(argc == 5) {
        sx = 0; sy = 0; sw = bw; sh = bh;
        dx = v[0]; dy = v[1]; dw = v[2]; dh = v[3];
    } else {
        sx = v[0]; sy = v[1]; sw = v[2]; sh = v[3];
        dx = v[4]; dy = v[5]; dw = v[6]; dh = v[7];
    }
    if(sw == 0 || sh == 0 || dw == 0 || dh == 0) return NULL;
    /* Transform the destination rect into device space. */
    float x0, y0, x1, y1;
    cv_xform(cv, dx, dy, &x0, &y0);
    cv_xform(cv, dx + dw, dy + dh, &x1, &y1);
    int ix = (int)(fminf(x0, x1) + 0.5f), iy = (int)(fminf(y0, y1) + 0.5f);
    int jx = (int)(fmaxf(x0, x1) + 0.5f), jy = (int)(fmaxf(y0, y1) + 0.5f);
    if(cv->has_clip) {
        if(ix < cv->clip_x) ix = cv->clip_x;
        if(iy < cv->clip_y) iy = cv->clip_y;
        if(jx > cv->clip_x + cv->clip_w) jx = cv->clip_x + cv->clip_w;
        if(jy > cv->clip_y + cv->clip_h) jy = cv->clip_y + cv->clip_h;
    }
    if(jx <= ix || jy <= iy) return NULL;
    uint8_t alpha = (uint8_t)(cv->globalAlpha * 255.0f + 0.5f);
    st->cb.blit(st->ctx, cv->canvas, bmp,
                (int)(sx + 0.5f), (int)(sy + 0.5f), (int)(sw + 0.5f), (int)(sh + 0.5f),
                ix, iy, jx - ix, jy - iy,
                alpha, cv->imageSmoothingEnabled);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ImageData                                                          */
/* ------------------------------------------------------------------ */

/* ImageData instance layout: ->value points at a heap cv_imagedata that
 * owns an embedder bitmap plus its dimensions. The `data` property is a
 * Uint8ClampedArray-like JS array of length w*h*4 (lazily materialized on
 * read; writing back is handled by putImageData reading the array). */
typedef struct {
    void* bitmap;       /* embedder-owned via bitmap_create */
    int   w, h;
    js_canvas_state* state;
} cv_imagedata;

static void js_imagedata_free(void* p) {
    cv_imagedata* id = (cv_imagedata*)p;
    if(id == NULL) return;
    if(id->bitmap != NULL && id->state != NULL && id->state->cb.bitmap_free != NULL)
        id->state->cb.bitmap_free(id->state->ctx, id->bitmap);
    mario_free(id);
}

static var_t* js_imagedata_get_width(vm_t* vm, var_t* env, void* data) {
    (void)data; var_t* t = get_obj(env, THIS);
    cv_imagedata* id = (t != NULL) ? (cv_imagedata*)t->value : NULL;
    return var_new_int(vm, id ? id->w : 0);
}
static var_t* js_imagedata_get_height(vm_t* vm, var_t* env, void* data) {
    (void)data; var_t* t = get_obj(env, THIS);
    cv_imagedata* id = (t != NULL) ? (cv_imagedata*)t->value : NULL;
    return var_new_int(vm, id ? id->h : 0);
}

/* Build the `data` array (w*h*4 bytes, RGBA order per spec) from the bitmap.
 * The embedder's bitmap is ARGB; we swizzle on read. */
static var_t* js_imagedata_get_data(vm_t* vm, var_t* env, void* data) {
    (void)data; var_t* t = get_obj(env, THIS);
    cv_imagedata* id = (t != NULL) ? (cv_imagedata*)t->value : NULL;
    var_t* arr = var_new_array(vm);
    if(arr == NULL || id == NULL) return arr;
    uint32_t n = (uint32_t)(id->w * id->h * 4);
    uint32_t* px = NULL;
    int pw = 0, ph = 0;
    if(id->state != NULL && id->state->cb.bitmap_data != NULL)
        px = id->state->cb.bitmap_data(id->state->ctx, id->bitmap, &pw, &ph);
    for(uint32_t i = 0; i < n; ++i) {
        uint8_t v = 0;
        if(px != NULL) {
            uint32_t p = px[i / 4];
            int comp = i % 4;
            if(comp == 0) v = (uint8_t)((p >> 16) & 0xFF);       /* R */
            else if(comp == 1) v = (uint8_t)((p >> 8) & 0xFF);   /* G */
            else if(comp == 2) v = (uint8_t)(p & 0xFF);          /* B */
            else v = (uint8_t)((p >> 24) & 0xFF);                /* A */
        }
        var_array_add(arr, var_new_int(vm, v));
    }
    return arr;
}

/* Write a JS data array back into the bitmap (used by putImageData). */
static void cv_imagedata_write(cv_imagedata* id, var_t* arr) {
    if(id == NULL || arr == NULL || id->state == NULL) return;
    uint32_t n = (uint32_t)(id->w * id->h * 4);
    uint32_t len = var_array_size(arr);
    if(len < n) n = len;
    uint32_t* px = NULL;
    int pw = 0, ph = 0;
    if(id->state->cb.bitmap_data != NULL)
        px = id->state->cb.bitmap_data(id->state->ctx, id->bitmap, &pw, &ph);
    for(uint32_t i = 0; i + 3 < n; i += 4) {
        node_t* nr = var_array_get(arr, (int32_t)i);
        node_t* ng = var_array_get(arr, (int32_t)i+1);
        node_t* nb = var_array_get(arr, (int32_t)i+2);
        node_t* na = var_array_get(arr, (int32_t)i+3);
        int r = nr ? var_get_int(nr->var) : 0;
        int g = ng ? var_get_int(ng->var) : 0;
        int b = nb ? var_get_int(nb->var) : 0;
        int a = na ? var_get_int(na->var) : 255;
        uint32_t c = ((uint32_t)clamp_u8(a) << 24) | ((uint32_t)clamp_u8(r) << 16) |
                     ((uint32_t)clamp_u8(g) << 8) | (uint32_t)clamp_u8(b);
        if(px != NULL) px[i / 4] = c;
        else if(id->state->cb.set_pixel != NULL) {
            int x = (int)((i / 4) % (uint32_t)id->w);
            int y = (int)((i / 4) / (uint32_t)id->w);
            id->state->cb.set_pixel(id->state->ctx, id->bitmap, x, y, c);
        }
    }
}

static cv_imagedata* cv_imagedata_new(js_canvas_state* st, int w, int h) {
    if(st == NULL || st->cb.bitmap_create == NULL || w <= 0 || h <= 0) return NULL;
    void* bmp = st->cb.bitmap_create(st->ctx, w, h);
    if(bmp == NULL) return NULL;
    cv_imagedata* id = (cv_imagedata*)mario_malloc(sizeof(cv_imagedata));
    if(id == NULL) {
        if(st->cb.bitmap_free != NULL) st->cb.bitmap_free(st->ctx, bmp);
        return NULL;
    }
    memset(id, 0, sizeof(*id));
    id->bitmap = bmp; id->w = w; id->h = h; id->state = st;
    return id;
}

static var_t* js_ctx_createImageData(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    var_t* args = get_func_args(env);
    uint32_t argc = var_array_size(args);
    int w = 0, h = 0;
    if(argc >= 2) {
        node_t* nw = var_array_get(args, 0);
        node_t* nh = var_array_get(args, 1);
        if(nw && nw->var) w = var_get_int(nw->var);
        if(nh && nh->var) h = var_get_int(nh->var);
    } else if(argc == 1) {
        /* createImageData(otherImageData) */
        node_t* n0 = var_array_get(args, 0);
        if(n0 && n0->var && n0->var->value) {
            cv_imagedata* src = (cv_imagedata*)n0->var->value;
            w = src->w; h = src->h;
        }
    }
    if(w <= 0 || h <= 0) return var_new_null(vm);
    cv_imagedata* id = cv_imagedata_new(st, w, h);
    if(id == NULL) return var_new_null(vm);
    var_t* obj = new_obj(vm, CLS_IMAGEDATA, 0);
    if(obj == NULL) { js_imagedata_free(id); return var_new_null(vm); }
    obj->value = (void*)id;
    obj->free_func = js_imagedata_free;
    return obj;
}

static var_t* js_ctx_getImageData(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || cv->canvas == NULL || st == NULL) return var_new_null(vm);
    int sx = (int)(get_float(env, "sx") + 0.5f);
    int sy = (int)(get_float(env, "sy") + 0.5f);
    int sw = (int)(get_float(env, "sw") + 0.5f);
    int sh = (int)(get_float(env, "sh") + 0.5f);
    if(sw <= 0 || sh <= 0) return var_new_null(vm);
    cv_imagedata* id = cv_imagedata_new(st, sw, sh);
    if(id == NULL) return var_new_null(vm);
    /* Copy pixels from the canvas into the new bitmap. */
    for(int y = 0; y < sh; ++y) {
        for(int x = 0; x < sw; ++x) {
            uint32_t c = 0;
            if(st->cb.get_pixel != NULL)
                c = st->cb.get_pixel(st->ctx, cv->canvas, sx + x, sy + y);
            if(st->cb.set_pixel != NULL)
                st->cb.set_pixel(st->ctx, id->bitmap, x, y, c);
        }
    }
    var_t* obj = new_obj(vm, CLS_IMAGEDATA, 0);
    if(obj == NULL) { js_imagedata_free(id); return var_new_null(vm); }
    obj->value = (void*)id;
    obj->free_func = js_imagedata_free;
    return obj;
}

static var_t* js_ctx_putImageData(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env);
    if(cv == NULL || cv->canvas == NULL || st == NULL) return NULL;
    var_t* args = get_func_args(env);
    node_t* n0 = var_array_get(args, 0);
    if(n0 == NULL || n0->var == NULL || n0->var->value == NULL) return NULL;
    cv_imagedata* id = (cv_imagedata*)n0->var->value;
    int dx = (int)(get_float(env, "dx") + 0.5f);
    int dy = (int)(get_float(env, "dy") + 0.5f);
    /* Optional dirty rect (args 3..6). */
    int dirtyX = 0, dirtyY = 0, dirtyW = id->w, dirtyH = id->h;
    if(var_array_size(args) >= 7) {
        node_t* n3 = var_array_get(args, 3);
        node_t* n4 = var_array_get(args, 4);
        node_t* n5 = var_array_get(args, 5);
        node_t* n6 = var_array_get(args, 6);
        if(n3 && n3->var) dirtyX = var_get_int(n3->var);
        if(n4 && n4->var) dirtyY = var_get_int(n4->var);
        if(n5 && n5->var) dirtyW = var_get_int(n5->var);
        if(n6 && n6->var) dirtyH = var_get_int(n6->var);
        if(dirtyX < 0) { dirtyW += dirtyX; dirtyX = 0; }
        if(dirtyY < 0) { dirtyH += dirtyY; dirtyY = 0; }
        if(dirtyX + dirtyW > id->w) dirtyW = id->w - dirtyX;
        if(dirtyY + dirtyH > id->h) dirtyH = id->h - dirtyY;
    }
    if(dirtyW <= 0 || dirtyH <= 0) return NULL;
    /* putImageData ignores the CTM and globalAlpha per spec; it writes raw
     * pixels. If the JS side mutated `data`, re-read it back into the bitmap
     * first. We detect mutation by checking whether the data array was
     * materialized (cached on the instance). For simplicity we always write
     * from the bitmap; JS-side data edits are picked up by the getter
     * re-reading the bitmap, which round-trips. */
    for(int y = 0; y < dirtyH; ++y) {
        for(int x = 0; x < dirtyW; ++x) {
            uint32_t c = 0;
            if(st->cb.get_pixel != NULL)
                c = st->cb.get_pixel(st->ctx, id->bitmap, dirtyX + x, dirtyY + y);
            if(st->cb.set_pixel != NULL)
                st->cb.set_pixel(st->ctx, cv->canvas, dx + dirtyX + x, dy + dirtyY + y, c);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Path2D                                                             */
/* ------------------------------------------------------------------ */

/* A Path2D holds the same subpath/point arrays as a ctx but no style or
 * CTM. fill(path)/stroke(path)/clip(path) on a ctx temporarily swap the
 * ctx's path with the Path2D's, paint, and swap back. */
typedef struct {
    cv_point_t* pts;    int pts_n, pts_cap;
    cv_sub_t*   subs;   int subs_n, subs_cap;
} cv_path2d;

static void js_path2d_free(void* p) {
    cv_path2d* pd = (cv_path2d*)p;
    if(pd == NULL) return;
    if(pd->pts != NULL) mario_free(pd->pts);
    if(pd->subs != NULL) mario_free(pd->subs);
    mario_free(pd);
}

/* Swap a ctx's path arrays with a Path2D's. Returns the ctx's original
 * arrays via the out params so the caller can restore them. */
static void ctx_swap_path(js_canvas_ctx* cv, cv_path2d* pd,
                          cv_point_t** o_pts, int* o_pts_n, int* o_pts_cap,
                          cv_sub_t** o_subs, int* o_subs_n, int* o_subs_cap) {
    *o_pts = cv->pts; *o_pts_n = cv->pts_n; *o_pts_cap = cv->pts_cap;
    *o_subs = cv->subs; *o_subs_n = cv->subs_n; *o_subs_cap = cv->subs_cap;
    cv->pts = pd->pts; cv->pts_n = pd->pts_n; cv->pts_cap = pd->pts_cap;
    cv->subs = pd->subs; cv->subs_n = pd->subs_n; cv->subs_cap = pd->subs_cap;
}
static void ctx_restore_path(js_canvas_ctx* cv, cv_path2d* pd,
                             cv_point_t* pts, int pts_n, int pts_cap,
                             cv_sub_t* subs, int subs_n, int subs_cap) {
    pd->pts = cv->pts; pd->pts_n = cv->pts_n; pd->pts_cap = cv->pts_cap;
    pd->subs = cv->subs; pd->subs_n = cv->subs_n; pd->subs_cap = cv->subs_cap;
    cv->pts = pts; cv->pts_n = pts_n; cv->pts_cap = pts_cap;
    cv->subs = subs; cv->subs_n = subs_n; cv->subs_cap = subs_cap;
}

/* Path2D methods mirror the ctx path methods but operate on the Path2D's
 * own arrays. To avoid duplicating every native, we temporarily point a
 * scratch ctx at the Path2D's arrays, run the shared implementation, and
 * copy back. The scratch ctx borrows the real ctx's CTM so user-space
 * coords tessellate identically. */
static js_canvas_ctx* g_scratch_cv = NULL;   /* reused across Path2D calls */

static js_canvas_ctx* path2d_scratch(js_canvas_ctx* donor) {
    if(g_scratch_cv == NULL) {
        g_scratch_cv = (js_canvas_ctx*)mario_malloc(sizeof(js_canvas_ctx));
        if(g_scratch_cv == NULL) return NULL;
        memset(g_scratch_cv, 0, sizeof(*g_scratch_cv));
        g_scratch_cv->fillStyle.kind = 0;
        g_scratch_cv->strokeStyle.kind = 0;
        g_scratch_cv->lineWidth = 1.0f;
        g_scratch_cv->a = 1; g_scratch_cv->d = 1;
    }
    /* Borrow the donor's CTM and style so tessellation matches. */
    g_scratch_cv->a = donor->a; g_scratch_cv->b = donor->b;
    g_scratch_cv->c = donor->c; g_scratch_cv->d = donor->d;
    g_scratch_cv->e = donor->e; g_scratch_cv->f = donor->f;
    g_scratch_cv->state = donor->state;
    g_scratch_cv->canvas = donor->canvas;
    return g_scratch_cv;
}

/* Generic Path2D method dispatcher: pull the Path2D from `this`, swap its
 * arrays into the scratch ctx, invoke the shared ctx native, swap back. */
typedef var_t* (*ctx_native_fn)(vm_t*, var_t*, void*);
static var_t* path2d_dispatch(vm_t* vm, var_t* env, ctx_native_fn fn) {
    var_t* t = get_obj(env, THIS);
    if(t == NULL || t->value == NULL) return NULL;
    cv_path2d* pd = (cv_path2d*)t->value;
    /* Find any live ctx to donate a CTM. Use the first registered one. */
    js_canvas_state* st = state_from_vm(vm);
    js_canvas_ctx* donor = (st != NULL && st->ctxs_n > 0) ? st->ctxs[0] : NULL;
    if(donor == NULL) return NULL;
    js_canvas_ctx* sc = path2d_scratch(donor);
    if(sc == NULL) return NULL;
    sc->pts = pd->pts; sc->pts_n = pd->pts_n; sc->pts_cap = pd->pts_cap;
    sc->subs = pd->subs; sc->subs_n = pd->subs_n; sc->subs_cap = pd->subs_cap;
    /* Build a fake env whose THIS points at the scratch ctx. mario's
     * get_obj(env, THIS) reads env's member; we temporarily swap the value. */
    var_t* saved_this = get_obj(env, THIS);
    void* saved_val = saved_this->value;
    saved_this->value = (void*)sc;
    var_t* r = fn(vm, env, NULL);
    saved_this->value = saved_val;
    pd->pts = sc->pts; pd->pts_n = sc->pts_n; pd->pts_cap = sc->pts_cap;
    pd->subs = sc->subs; pd->subs_n = sc->subs_n; pd->subs_cap = sc->subs_cap;
    sc->pts = NULL; sc->pts_n = sc->pts_cap = 0;
    sc->subs = NULL; sc->subs_n = sc->subs_cap = 0;
    return r;
}

static var_t* js_path2d_ctor(vm_t* vm, var_t* env, void* data) {
    (void)data;
    cv_path2d* pd = (cv_path2d*)mario_malloc(sizeof(cv_path2d));
    if(pd == NULL) return NULL;
    memset(pd, 0, sizeof(*pd));
    /* If constructed from another Path2D or a path string, copy/parse. The
     * string-SVG form is out of scope; we accept a source Path2D. */
    var_t* args = get_func_args(env);
    node_t* n0 = var_array_get(args, 0);
    if(n0 != NULL && n0->var != NULL && n0->var->value != NULL) {
        cv_path2d* src = (cv_path2d*)n0->var->value;
        pd->pts_n = src->pts_n; pd->pts_cap = src->pts_cap;
        pd->subs_n = src->subs_n; pd->subs_cap = src->subs_cap;
        if(src->pts_n > 0) {
            pd->pts = (cv_point_t*)mario_malloc((uint32_t)(src->pts_cap * sizeof(cv_point_t)));
            if(pd->pts) memcpy(pd->pts, src->pts, (size_t)src->pts_n * sizeof(cv_point_t));
        }
        if(src->subs_n > 0) {
            pd->subs = (cv_sub_t*)mario_malloc((uint32_t)(src->subs_cap * sizeof(cv_sub_t)));
            if(pd->subs) memcpy(pd->subs, src->subs, (size_t)src->subs_n * sizeof(cv_sub_t));
        }
    }
    var_t* obj = new_obj(vm, CLS_PATH2D, 0);
    if(obj == NULL) { js_path2d_free(pd); return NULL; }
    obj->value = (void*)pd;
    obj->free_func = js_path2d_free;
    return obj;
}

#define PATH2D_METHOD(name, impl) \
    static var_t* js_path2d_##name(vm_t* vm, var_t* env, void* data) { \
        (void)data; return path2d_dispatch(vm, env, impl); \
    }
PATH2D_METHOD(beginPath, js_ctx_beginPath)
PATH2D_METHOD(closePath, js_ctx_closePath)
PATH2D_METHOD(moveTo, js_ctx_moveTo)
PATH2D_METHOD(lineTo, js_ctx_lineTo)
PATH2D_METHOD(quadraticCurveTo, js_ctx_quadraticCurveTo)
PATH2D_METHOD(bezierCurveTo, js_ctx_bezierCurveTo)
PATH2D_METHOD(arcTo, js_ctx_arcTo)
PATH2D_METHOD(rect, js_ctx_rect)
PATH2D_METHOD(roundRect, js_ctx_roundRect)
PATH2D_METHOD(arc, js_ctx_arc)
PATH2D_METHOD(ellipse, js_ctx_ellipse)

static var_t* js_path2d_addPath(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* t = get_obj(env, THIS);
    if(t == NULL || t->value == NULL) return NULL;
    cv_path2d* pd = (cv_path2d*)t->value;
    var_t* args = get_func_args(env);
    node_t* n0 = var_array_get(args, 0);
    if(n0 == NULL || n0->var == NULL || n0->var->value == NULL) return NULL;
    cv_path2d* src = (cv_path2d*)n0->var->value;
    /* Optional transform arg (DOMMatrix) is ignored for simplicity. */
    int base = pd->pts_n;
    for(int i = 0; i < src->pts_n; ++i) {
        pd->pts = (cv_point_t*)cv_grow(pd->pts, &pd->pts_cap, pd->pts_n + 1, sizeof(cv_point_t));
        if(pd->pts_cap < pd->pts_n + 1) break;
        pd->pts[pd->pts_n++] = src->pts[i];
    }
    for(int i = 0; i < src->subs_n; ++i) {
        pd->subs = (cv_sub_t*)cv_grow(pd->subs, &pd->subs_cap, pd->subs_n + 1, sizeof(cv_sub_t));
        if(pd->subs_cap < pd->subs_n + 1) break;
        cv_sub_t s = src->subs[i];
        s.pts_start += base;
        pd->subs[pd->subs_n++] = s;
    }
    return NULL;
}

/* fill(path) / stroke(path) / clip(path) overloads: if arg0 is a Path2D,
 * swap it in, paint, swap back. */
static var_t* js_ctx_fill_with_path(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    int sx = 0, sy = 0;
    int have_shadow = cv_shadow_offsets(cv, &sx, &sy);
    var_t* args = get_func_args(env);
    node_t* n0 = var_array_get(args, 0);
    if(n0 != NULL && n0->var != NULL && n0->var->value != NULL) {
        var_t* proto = var_get_prototype(n0->var);
        const char* cname = "";
        if(proto != NULL) {
            const char* s = var_get_str(var_find_own_member_var(proto, "__class_name"));
            if(s != NULL) cname = s;
        }
        if(!strcmp(cname, CLS_PATH2D)) {
            cv_path2d* pd = (cv_path2d*)n0->var->value;
            cv_point_t* op; int opn, opc; cv_sub_t* os; int osn, osc;
            ctx_swap_path(cv, pd, &op, &opn, &opc, &os, &osn, &osc);
            if(have_shadow) {
                cv_style sh; sh.kind = 0; sh.color = cv->shadowColor; sh.obj = NULL;
                cv_fill_path(st, cv, &sh, sx, sy);
            }
            cv_fill_path(st, cv, &cv->fillStyle, 0, 0);
            ctx_restore_path(cv, pd, op, opn, opc, os, osn, osc);
            return NULL;
        }
    }
    if(have_shadow) {
        cv_style sh; sh.kind = 0; sh.color = cv->shadowColor; sh.obj = NULL;
        cv_fill_path(st, cv, &sh, sx, sy);
    }
    cv_fill_path(st, cv, &cv->fillStyle, 0, 0);
    return NULL;
}

static var_t* js_ctx_stroke_with_path(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    js_canvas_ctx* cv = ctx_from_env(env); if(cv == NULL) return NULL;
    int sx = 0, sy = 0;
    int have_shadow = cv_shadow_offsets(cv, &sx, &sy);
    var_t* args = get_func_args(env);
    node_t* n0 = var_array_get(args, 0);
    if(n0 != NULL && n0->var != NULL && n0->var->value != NULL) {
        var_t* proto = var_get_prototype(n0->var);
        const char* cname = "";
        if(proto != NULL) {
            const char* s = var_get_str(var_find_own_member_var(proto, "__class_name"));
            if(s != NULL) cname = s;
        }
        if(!strcmp(cname, CLS_PATH2D)) {
            cv_path2d* pd = (cv_path2d*)n0->var->value;
            cv_point_t* op; int opn, opc; cv_sub_t* os; int osn, osc;
            ctx_swap_path(cv, pd, &op, &opn, &opc, &os, &osn, &osc);
            if(have_shadow) {
                cv_style sh; sh.kind = 0; sh.color = cv->shadowColor; sh.obj = NULL;
                cv_stroke_path(st, cv, &sh, sx, sy);
            }
            cv_stroke_path(st, cv, &cv->strokeStyle, 0, 0);
            ctx_restore_path(cv, pd, op, opn, opc, os, osn, osc);
            return NULL;
        }
    }
    if(have_shadow) {
        cv_style sh; sh.kind = 0; sh.color = cv->shadowColor; sh.obj = NULL;
        cv_stroke_path(st, cv, &sh, sx, sy);
    }
    cv_stroke_path(st, cv, &cv->strokeStyle, 0, 0);
    return NULL;
}


/* ------------------------------------------------------------------ */
/* Element.getContext('2d') + width/height                            */
/*                                                                    */
/* These hang on the Element class js_dom.c created; `this->value` is  */
/* the opaque element handle js_dom stashed there. getContext resolves */
/* the id/size, asks the embedder for the backing store, and wraps the */
/* cached per-canvas context in a CanvasRenderingContext2D instance.   */
/* ------------------------------------------------------------------ */

static void js_ctx_noop_free(void* p) { (void)p; }

/* Read an integer attribute via the embedder, falling back to `def`. Frees
 * the mario_malloc'd string el_get_attr hands back. */
static int el_int_attr(js_canvas_state* st, void* el, const char* name, int def) {
    if(st == NULL || st->cb.el_get_attr == NULL || el == NULL) return def;
    char* s = st->cb.el_get_attr(st->ctx, el, name);
    if(s == NULL) return def;
    int v = atoi(s);
    mario_free(s);
    return (v > 0) ? v : def;
}

static var_t* js_el_getContext(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    if(st == NULL || st->cb.canvas_create == NULL) return var_new_null(vm);
    var_t* t = get_obj(env, THIS);
    if(t == NULL || t->value == NULL) return var_new_null(vm);
    void* el = t->value;

    /* Per spec, getContext returns null for an unsupported context id. We
     * only support "2d". */
    const char* type = get_str(env, "type");
    if(type != NULL && type[0] != 0 && strcmp(type, "2d") != 0)
        return var_new_null(vm);

    int w = el_int_attr(st, el, "width", 300);
    int h = el_int_attr(st, el, "height", 150);

    /* Key the canvas by element id. With no id there is nothing to composite
     * by, so synthesize a stable key from the element pointer; getContext
     * still yields a usable bitmap, but the embedder will not blit it (no
     * selector finds it). The classic demos give their canvas an id. */
    char idbuf[48];
    const char* id = NULL;
    if(st->cb.el_get_attr != NULL) id = st->cb.el_get_attr(st->ctx, el, "id");
    if(id == NULL || id[0] == 0) {
        snprintf(idbuf, sizeof(idbuf), "@cv%p", el);
        if(id != NULL) mario_free((void*)id);
        id = idbuf;
    }
    void* canvas = st->cb.canvas_create(st->ctx, id, w, h);
    if(id != idbuf && id != NULL) mario_free((void*)id);
    if(canvas == NULL) return var_new_null(vm);

    js_canvas_ctx* cv = ctx_for_canvas(st, canvas, w, h);
    if(cv == NULL) return var_new_null(vm);
    /* Keep the cached dims in sync if the markup changed. */
    cv->canvas_w = w; cv->canvas_h = h;

    var_t* obj = new_obj(vm, CLS_CTX2D, 0);
    if(obj == NULL) return var_new_null(vm);
    obj->value = (void*)cv;
    obj->free_func = js_ctx_noop_free;
    return obj;
}

static var_t* js_el_get_width(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    var_t* t = get_obj(env, THIS);
    void* el = (t != NULL) ? t->value : NULL;
    return var_new_int(vm, el_int_attr(st, el, "width", 300));
}
static var_t* js_el_get_height(vm_t* vm, var_t* env, void* data) {
    js_canvas_state* st = state_any(vm, data);
    var_t* t = get_obj(env, THIS);
    void* el = (t != NULL) ? t->value : NULL;
    return var_new_int(vm, el_int_attr(st, el, "height", 150));
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

/* Tag a class prototype with its name so natives can recover the class of an
 * instance via var_find_own_member_var(var_get_prototype(v), "__class_name").
 * mario does not expose the class name on instances, and the bridge needs it
 * to distinguish CanvasGradient / CanvasPattern / ImageData / Path2D values
 * passed to fillStyle / drawImage / createPattern / fill(path). */
static void tag_class(vm_t* vm, var_t* cls, const char* name) {
    if(cls == NULL) return;
    var_t* proto = var_get_prototype(cls);
    if(proto == NULL) return;
    var_add(proto, "__class_name", var_new_str(vm, name));
}

bool js_register_canvas_natives(vm_t* vm, void* ctx, const js_canvas_callbacks_t* cb) {
    if(vm == NULL || cb == NULL) return false;

    js_canvas_state* st = (js_canvas_state*)mario_malloc(sizeof(js_canvas_state));
    if(st == NULL) return false;
    memset(st, 0, sizeof(*st));
    st->ctx = ctx;
    st->cb = *cb;
    st->vm = vm;

    var_t* bridge = var_new_obj_no_proto(vm, st, canvas_state_free);
    if(bridge == NULL) { mario_free(st); return false; }
    var_add(vm->root, CANVAS_BRIDGE_KEY, bridge);

    /* ---- CanvasRenderingContext2D ---- */
    var_t* ctx_cls = vm_new_class(vm, CLS_CTX2D);
    tag_class(vm, ctx_cls, CLS_CTX2D);
    if(ctx_cls != NULL) {
        /* Path construction. */
        vm_reg_native(vm, ctx_cls, "beginPath()",                          js_ctx_beginPath,        bridge);
        vm_reg_native(vm, ctx_cls, "closePath()",                          js_ctx_closePath,        bridge);
        vm_reg_native(vm, ctx_cls, "moveTo(x, y)",                         js_ctx_moveTo,           bridge);
        vm_reg_native(vm, ctx_cls, "lineTo(x, y)",                         js_ctx_lineTo,           bridge);
        vm_reg_native(vm, ctx_cls, "quadraticCurveTo(cpx, cpy, x, y)",     js_ctx_quadraticCurveTo, bridge);
        vm_reg_native(vm, ctx_cls, "bezierCurveTo(cp1x, cp1y, cp2x, cp2y, x, y)", js_ctx_bezierCurveTo, bridge);
        vm_reg_native(vm, ctx_cls, "arcTo(x1, y1, x2, y2, r)",             js_ctx_arcTo,            bridge);
        vm_reg_native(vm, ctx_cls, "rect(x, y, w, h)",                     js_ctx_rect,             bridge);
        vm_reg_native(vm, ctx_cls, "roundRect(x, y, w, h, r)",             js_ctx_roundRect,        bridge);
        vm_reg_native(vm, ctx_cls, "arc(x, y, r, start, end)",             js_ctx_arc,              bridge);
        vm_reg_native(vm, ctx_cls, "ellipse(x, y, rx, ry, rot, start, end)", js_ctx_ellipse,        bridge);
        /* Paint (fill/stroke accept an optional Path2D first arg). */
        vm_reg_native(vm, ctx_cls, "fill()",                               js_ctx_fill_with_path,   bridge);
        vm_reg_native(vm, ctx_cls, "stroke()",                             js_ctx_stroke_with_path, bridge);
        vm_reg_native(vm, ctx_cls, "clip()",                               js_ctx_clip,             bridge);
        vm_reg_native(vm, ctx_cls, "isPointInPath(x, y)",                  js_ctx_isPointInPath,    bridge);
        vm_reg_native(vm, ctx_cls, "isPointInStroke(x, y)",                js_ctx_isPointInStroke,  bridge);
        vm_reg_native(vm, ctx_cls, "clearRect(x, y, w, h)",                js_ctx_clearRect,        bridge);
        vm_reg_native(vm, ctx_cls, "fillRect(x, y, w, h)",                 js_ctx_fillRect,         bridge);
        vm_reg_native(vm, ctx_cls, "strokeRect(x, y, w, h)",               js_ctx_strokeRect,       bridge);
        /* Text. */
        vm_reg_native(vm, ctx_cls, "fillText(text, x, y)",                 js_ctx_fillText,         bridge);
        vm_reg_native(vm, ctx_cls, "strokeText(text, x, y)",               js_ctx_strokeText,       bridge);
        vm_reg_native(vm, ctx_cls, "measureText(text)",                    js_ctx_measureText,      bridge);
        /* State + transform. */
        vm_reg_native(vm, ctx_cls, "save()",                               js_ctx_save,             bridge);
        vm_reg_native(vm, ctx_cls, "restore()",                            js_ctx_restore,          bridge);
        vm_reg_native(vm, ctx_cls, "reset()",                              js_ctx_reset,            bridge);
        vm_reg_native(vm, ctx_cls, "translate(x, y)",                      js_ctx_translate,        bridge);
        vm_reg_native(vm, ctx_cls, "rotate(a)",                            js_ctx_rotate,           bridge);
        vm_reg_native(vm, ctx_cls, "scale(x, y)",                          js_ctx_scale,            bridge);
        vm_reg_native(vm, ctx_cls, "transform(a, b, c, d, e, f)",          js_ctx_transform,        bridge);
        vm_reg_native(vm, ctx_cls, "setTransform(a, b, c, d, e, f)",       js_ctx_setTransform,     bridge);
        vm_reg_native(vm, ctx_cls, "resetTransform()",                     js_ctx_resetTransform,   bridge);
        vm_reg_native(vm, ctx_cls, "getTransform()",                       js_ctx_getTransform,     bridge);
        /* Line dash. */
        vm_reg_native(vm, ctx_cls, "setLineDash(segments)",                js_ctx_setLineDash,      bridge);
        vm_reg_native(vm, ctx_cls, "getLineDash()",                        js_ctx_getLineDash,      bridge);
        /* Gradients / patterns. */
        vm_reg_native(vm, ctx_cls, "createLinearGradient(x0, y0, x1, y1)", js_ctx_createLinearGradient, bridge);
        vm_reg_native(vm, ctx_cls, "createRadialGradient(x0, y0, r0, x1, y1, r1)", js_ctx_createRadialGradient, bridge);
        vm_reg_native(vm, ctx_cls, "createConicGradient(startAngle, x, y)", js_ctx_createConicGradient, bridge);
        vm_reg_native(vm, ctx_cls, "createPattern(image, repetition)",     js_ctx_createPattern,    bridge);
        /* Images + pixels. */
        vm_reg_native(vm, ctx_cls, "drawImage(image, a, b, c, d, e, f, g, h)", js_ctx_drawImage,    bridge);
        vm_reg_native(vm, ctx_cls, "createImageData(a, b)",                js_ctx_createImageData,  bridge);
        vm_reg_native(vm, ctx_cls, "getImageData(sx, sy, sw, sh)",         js_ctx_getImageData,     bridge);
        vm_reg_native(vm, ctx_cls, "putImageData(imagedata, dx, dy)",      js_ctx_putImageData,     bridge);
        /* Misc. */
        vm_reg_native(vm, ctx_cls, "getContextAttributes()",               js_ctx_getContextAttributes, bridge);

        /* Style accessors. */
        reg_accessor(vm, ctx_cls, "fillStyle",                js_ctx_get_fillStyle,                js_ctx_set_fillStyle);
        reg_accessor(vm, ctx_cls, "strokeStyle",              js_ctx_get_strokeStyle,              js_ctx_set_strokeStyle);
        reg_accessor(vm, ctx_cls, "lineWidth",                js_ctx_get_lineWidth,                js_ctx_set_lineWidth);
        reg_accessor(vm, ctx_cls, "lineCap",                  js_ctx_get_lineCap,                  js_ctx_set_lineCap);
        reg_accessor(vm, ctx_cls, "lineJoin",                 js_ctx_get_lineJoin,                 js_ctx_set_lineJoin);
        reg_accessor(vm, ctx_cls, "miterLimit",               js_ctx_get_miterLimit,               js_ctx_set_miterLimit);
        reg_accessor(vm, ctx_cls, "lineDashOffset",           js_ctx_get_lineDashOffset,           js_ctx_set_lineDashOffset);
        reg_accessor(vm, ctx_cls, "globalAlpha",              js_ctx_get_globalAlpha,              js_ctx_set_globalAlpha);
        reg_accessor(vm, ctx_cls, "globalCompositeOperation", js_ctx_get_globalCompositeOperation, js_ctx_set_globalCompositeOperation);
        reg_accessor(vm, ctx_cls, "imageSmoothingEnabled",    js_ctx_get_imageSmoothingEnabled,    js_ctx_set_imageSmoothingEnabled);
        reg_accessor(vm, ctx_cls, "imageSmoothingQuality",    js_ctx_get_imageSmoothingQuality,    js_ctx_set_imageSmoothingQuality);
        reg_accessor(vm, ctx_cls, "shadowBlur",               js_ctx_get_shadowBlur,               js_ctx_set_shadowBlur);
        reg_accessor(vm, ctx_cls, "shadowColor",              js_ctx_get_shadowColor,              js_ctx_set_shadowColor);
        reg_accessor(vm, ctx_cls, "shadowOffsetX",            js_ctx_get_shadowOffsetX,            js_ctx_set_shadowOffsetX);
        reg_accessor(vm, ctx_cls, "shadowOffsetY",            js_ctx_get_shadowOffsetY,            js_ctx_set_shadowOffsetY);
        reg_accessor(vm, ctx_cls, "font",                     js_ctx_get_font,                     js_ctx_set_font);
        reg_accessor(vm, ctx_cls, "textAlign",                js_ctx_get_textAlign,                js_ctx_set_textAlign);
        reg_accessor(vm, ctx_cls, "textBaseline",             js_ctx_get_textBaseline,             js_ctx_set_textBaseline);
        reg_accessor(vm, ctx_cls, "direction",                js_ctx_get_direction,                js_ctx_set_direction);
        reg_accessor(vm, ctx_cls, "letterSpacing",            js_ctx_get_letterSpacing,            js_ctx_set_letterSpacing);
        reg_accessor(vm, ctx_cls, "wordSpacing",              js_ctx_get_wordSpacing,              js_ctx_set_wordSpacing);
        reg_accessor(vm, ctx_cls, "fontKerning",              js_ctx_get_fontKerning,              js_ctx_set_fontKerning);
        reg_accessor(vm, ctx_cls, "fontStretch",              js_ctx_get_fontStretch,              js_ctx_set_fontStretch);
        reg_accessor(vm, ctx_cls, "fontVariantCaps",          js_ctx_get_fontVariantCaps,          js_ctx_set_fontVariantCaps);
        reg_accessor(vm, ctx_cls, "textRendering",            js_ctx_get_textRendering,            js_ctx_set_textRendering);
        reg_accessor(vm, ctx_cls, "filter",                   js_ctx_get_filter,                   js_ctx_set_filter);
        reg_accessor(vm, ctx_cls, "canvas",                   js_ctx_get_canvas,                   NULL);
    }

    /* ---- CanvasGradient ---- */
    var_t* grad_cls = vm_new_class(vm, CLS_GRADIENT);
    tag_class(vm, grad_cls, CLS_GRADIENT);
    if(grad_cls != NULL) {
        vm_reg_native(vm, grad_cls, "addColorStop(offset, color)", js_grad_addColorStop, NULL);
    }

    /* ---- CanvasPattern ---- */
    var_t* pat_cls = vm_new_class(vm, CLS_PATTERN);
    tag_class(vm, pat_cls, CLS_PATTERN);
    if(pat_cls != NULL) {
        vm_reg_native(vm, pat_cls, "setTransform(matrix)", js_pat_setTransform, NULL);
    }

    /* ---- ImageData ---- */
    var_t* id_cls = vm_new_class(vm, CLS_IMAGEDATA);
    tag_class(vm, id_cls, CLS_IMAGEDATA);
    if(id_cls != NULL) {
        reg_accessor(vm, id_cls, "width",  js_imagedata_get_width,  NULL);
        reg_accessor(vm, id_cls, "height", js_imagedata_get_height, NULL);
        reg_accessor(vm, id_cls, "data",   js_imagedata_get_data,   NULL);
    }

    /* ---- TextMetrics / DOMMatrix: plain marker classes; their instances are
     * built field-by-field in measureText / getTransform, so no natives are
     * registered here beyond the class tag used for type checks. ---- */
    tag_class(vm, vm_new_class(vm, CLS_TEXTMETRICS), CLS_TEXTMETRICS);
    tag_class(vm, vm_new_class(vm, CLS_DOMMATRIX),   CLS_DOMMATRIX);

    /* ---- Path2D ---- */
    var_t* p2d_cls = vm_new_class(vm, CLS_PATH2D);
    tag_class(vm, p2d_cls, CLS_PATH2D);
    if(p2d_cls != NULL) {
        /* The constructor is invoked via `new Path2D(...)`. mario dispatches
         * native-class construction through the prototype's constructor
         * member; register it under CONSTRUCTOR so new_obj finds it. */
        vm_reg_native(vm, p2d_cls, "Path2D()",                js_path2d_ctor,           NULL);
        vm_reg_native(vm, p2d_cls, "beginPath()",             js_path2d_beginPath,      NULL);
        vm_reg_native(vm, p2d_cls, "closePath()",             js_path2d_closePath,      NULL);
        vm_reg_native(vm, p2d_cls, "moveTo(x, y)",            js_path2d_moveTo,         NULL);
        vm_reg_native(vm, p2d_cls, "lineTo(x, y)",            js_path2d_lineTo,         NULL);
        vm_reg_native(vm, p2d_cls, "quadraticCurveTo(cpx, cpy, x, y)", js_path2d_quadraticCurveTo, NULL);
        vm_reg_native(vm, p2d_cls, "bezierCurveTo(cp1x, cp1y, cp2x, cp2y, x, y)", js_path2d_bezierCurveTo, NULL);
        vm_reg_native(vm, p2d_cls, "arcTo(x1, y1, x2, y2, r)", js_path2d_arcTo,         NULL);
        vm_reg_native(vm, p2d_cls, "rect(x, y, w, h)",        js_path2d_rect,           NULL);
        vm_reg_native(vm, p2d_cls, "roundRect(x, y, w, h, r)", js_path2d_roundRect,     NULL);
        vm_reg_native(vm, p2d_cls, "arc(x, y, r, start, end)", js_path2d_arc,           NULL);
        vm_reg_native(vm, p2d_cls, "ellipse(x, y, rx, ry, rot, start, end)", js_path2d_ellipse, NULL);
        vm_reg_native(vm, p2d_cls, "addPath(path)",           js_path2d_addPath,        NULL);
    }

    /* getContext() + width/height hang on the Element class js_dom.c already
     * created; registered with data = bridge so state_any() recovers st. */
    var_t* el_cls = var_find_own_member_var(vm->root, CLS_ELEMENT);
    if(el_cls != NULL) {
        vm_reg_native(vm, el_cls, "getContext(type)", js_el_getContext, bridge);
        reg_accessor(vm, el_cls, "width",  js_el_get_width,  NULL);
        reg_accessor(vm, el_cls, "height", js_el_get_height, NULL);
    }

    return true;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */
