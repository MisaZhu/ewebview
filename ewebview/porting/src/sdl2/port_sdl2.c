/*
 * port_sdl2.c - the desktop SDL2 implementation of the ewebview HAL.
 *
 * Maps every ewebview porting table onto SDL2 + SDL2_gfx primitives, with the
 * standard SDL2 companion libraries filling the gaps SDL2 core does not cover:
 *   eweb_surface_t  -> sdl_surf_t { SDL_Surface (ARGB8888) + SDL_Renderer }
 *                      The software renderer lets every SDL2_gfx primitive
 *                      (line/box/circle/arc/pie/rounded/bezier/...) target the
 *                      surface directly; SDL_FillRect / SDL_BlitSurface /
 *                      SDL_BlitScaled handle the bulk pixel paths.
 *   eweb_font_t     -> sdl_font_t { family + TTF_Font* cache keyed by size }
 *                      SDL2_ttf bakes the point size into the handle while the
 *                      HAL passes size per call, so we keep a small LRU-free
 *                      per-size cache inside one eweb_font_t.
 *   image.decode    -> SDL2_image (IMG_Load_RW) + ConvertSurfaceFormat(ARGB8888);
 *                      SVG instead keeps its parsed plutosvg document in the
 *                      handle and re-rasterises at the blit dst device size,
 *                      so scaled vector art stays smooth at any ratio
 *   HiDPI           -> eweb_port_sdl2_set_dpr(): surface_new allocates logical
 *                      size * dpr device pixels and every draw/font callback
 *                      scales logical coords up, so layout stays in CSS pixels
 *                      while rasterisation runs at native device resolution.
 *   net.request     -> libtinyhttpsc (BearSSL HTTP/HTTPS), one hop per call.
 *                      Kept identical to the EwokOS reference port: tinyhttpsc
 *                      lives in browser/ewebview/libtinyhttpsc/ and is a
 *                      portable BearSSL client with no OS-specific deps, so it
 *                      works on desktop builds unchanged.
 *   net.read_file   -> standard C fopen/fread (file://)
 *   net.resolve_res -> <program-dir>/res/<name> (res://, via SDL_GetBasePath)
 *   clock.tic_ms    -> clock_gettime(CLOCK_MONOTONIC), matching litehtml's
 *                      internal sys_tic_ms() so absolute deadline comparisons
 *                      in the chunked style walk share one time base
 *   clock.sleep_ms  -> SDL_Delay
 *   sys.log         -> SDL_Log
 *   sys.ptr_sane    -> non-NULL heuristic (desktop OSes expose no cheap
 *                      heap-membership test; OPTIONAL, the core degrades to
 *                      trusting the liveness tag alone)
 *
 * Threading mirrors the reference port: gfx/font run on the engine thread,
 * net + image.decode run on a download worker thread, clock runs on both.
 * SDL2 surfaces/renderers are NOT thread-safe, but each surface is only ever
 * touched by the thread that created it, and SDL2_ttf/SDL2_image/SDL_net are
 * re-entrant for independent handles.
 *
 * Build: link against -lSDL2 -lSDL2_gfx -lSDL2_ttf -lSDL2_image plus
 * libtinyhttpsc (+ BearSSL). pkg-config names: sdl2 SDL2_ttf SDL2_image
 * SDL2_gfx. The embedder must have called SDL_Init(SDL_INIT_VIDEO) before
 * ewebview_create(); TTF_Init / IMG_Init are done lazily here.
 *
 * Plain C99.
 */

#include <ewebview_port.h>

/* ---- SDL2 core + companion libraries ------------------------------ */
#include <SDL.h>
#include <SDL2_gfxPrimitives.h>
#include <SDL2_rotozoom.h>
#include <SDL_ttf.h>
#include <SDL_image.h>

/* ---- SVG vector decode/re-raster (libsvg: plutosvg + plutovg) ------ */
#include <svg.h>

/* ---- Network: unchanged from the EwokOS reference port ------------ */
/* libtinyhttpsc is a portable BearSSL HTTP/HTTPS client that ships inside
 * browser/ewebview/libtinyhttpsc/; it has no OS-specific dependencies, so the
 * desktop SDL2 port reuses it verbatim instead of hand-rolling an HTTP client
 * on top of SDL_net (which has no TLS story anyway). */
#include <tinyhttpsc/tinyhttpsc.h>
#include <tinyhttpsc/BearHttpsClientOne.h>

/* ---- Standard C --------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Handle <-> concrete type                                            */
/* ------------------------------------------------------------------ */

/* One eweb_surface_t == one SDL_Surface plus a software renderer that targets
 * it. The renderer exists purely so SDL2_gfx's primitive set (which is
 * renderer-based in SDL2) can draw into the surface; bulk pixel paths
 * (fill_rect / clear / blit) bypass it and hit the surface directly. */
typedef struct {
    SDL_Surface*  surf;
    SDL_Renderer* rend;   /* may be NULL for decoded images (blit sources only) */
    /* HiDPI: the pixel buffer holds lw x lh LOGICAL pixels at `dpr` device
     * pixels per logical pixel (surf->w/h == lw*dpr x lh*dpr). The core lays
     * out and draws in logical pixels (surface_dims reports lw/lh); every draw
     * callback scales by the TARGET surface's own dpr, so a surface keeps
     * rendering consistently even if the global ratio changes later. Decoded
     * images are natural-pixel buffers with dpr == 1. */
    int           lw, lh;
    float         dpr;
    /* SVG vector source: the parsed document stays alive so a scaled blit
     * can re-rasterise at the exact dst device size - resampling the fixed
     * intrinsic raster (SDL_BlitScaled) aliases when shrinking, while the
     * vector render is anti-aliased at whatever size is asked for.
     * svg_cache memoises the last rendered size so steady-state repaints
     * and scrolling never re-render. Both are engine-thread only (blits
     * and surface_free run there; decode only creates the doc). */
    svg_doc_t*    svg_doc;
    SDL_Surface*  svg_cache;
    int           svg_cache_w, svg_cache_h;
} sdl_surf_t;

#define S(h)  ((sdl_surf_t*)(h))
#define SH(p) ((eweb_surface_t*)(p))

/* SDL2_ttf bakes the point size into TTF_Font*, while the HAL passes size per
 * call. One eweb_font_t therefore owns a small per-size face cache. Typical
 * pages use <=8 distinct sizes, so 24 slots is plenty and never needs eviction. */
#define SDL_FONT_MAX_FACES 24
typedef struct {
    int       size;
    TTF_Font* face;
} sdl_face_slot_t;

typedef struct {
    char            family[64];
    sdl_face_slot_t faces[SDL_FONT_MAX_FACES];
    int             face_count;
} sdl_font_t;

#define F(h)  ((sdl_font_t*)(h))
#define FH(p) ((eweb_font_t*)(p))

/* ------------------------------------------------------------------ */
/* HiDPI device pixel ratio                                            */
/* ------------------------------------------------------------------ */

/* Embedder-settable ratio of device pixels per logical (CSS) pixel, e.g. 2 on
 * a Retina panel. surface_new() allocates buffers at logical size * dpr and
 * every draw/font callback scales logical coordinates up by the target
 * surface's dpr, so the engine lays out at logical size (normal text metrics)
 * while rasterising at native resolution (crisp text and hairlines, no page
 * zoom). dpr == 1 keeps every path numerically identical to a plain 1x port. */
static float g_dpr = 1.0f;

void eweb_port_sdl2_set_dpr(float dpr) {
    g_dpr = (dpr > 0.0f) ? dpr : 1.0f;
}

/* Scale one logical coordinate/length into a surface's device pixels. */
static inline int sdl2_sp(const sdl_surf_t* s, int v) {
    return (int)lroundf((float)v * s->dpr);
}

/* Scale a logical font size into device pixels for rasterisation. */
static inline int sdl2_font_px_dpr(int size, float dpr) {
    int d = (int)lroundf((float)size * dpr);
    return d < 1 ? 1 : d;
}
static inline int sdl2_font_px(int size) { return sdl2_font_px_dpr(size, g_dpr); }

/* Map a device-pixel font measurement back to the logical pixels the layout
 * sees (rounded, so logical metrics stay integral like the HAL requires). */
static inline int sdl2_logical_px(int dev) {
    return (int)lroundf((float)dev / g_dpr);
}

/* ------------------------------------------------------------------ */
/* Lazy companion-lib init                                             */
/* ------------------------------------------------------------------ */

static bool s_libs_ready = false;

/* TTF_Init / IMG_Init are cheap and idempotent; call once on first use. The
 * embedder is expected to have called SDL_Init(SDL_INIT_VIDEO) already (every
 * SDL2 app does), but we nudge it here in case a headless harness forgot. */
static bool sdl2_ensure_libs(void) {
    if(s_libs_ready) return true;
    if(!(SDL_WasInit(SDL_INIT_VIDEO))) {
        if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
            SDL_Log("port_sdl2: SDL_Init failed: %s", SDL_GetError());
            return false;
        }
    }
    if(TTF_Init() < 0) {
        SDL_Log("port_sdl2: TTF_Init failed: %s", TTF_GetError());
        return false;
    }
    /* PNG/JPG/WEBP cover the realistic web image set; ignore failure (the
     * formats simply degrade to unsupported). */
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);
    s_libs_ready = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

/* ewebview colors are 0xAARRGGBB; SDL2_gfx's *RGBA primitives want the four
 * components unpacked. */
static inline void sdl2_unpack_argb(uint32_t c, Uint8* r, Uint8* g, Uint8* b, Uint8* a) {
    *a = (Uint8)((c >> 24) & 0xFF);
    *r = (Uint8)((c >> 16) & 0xFF);
    *g = (Uint8)((c >> 8)  & 0xFF);
    *b = (Uint8)( c        & 0xFF);
}

/* Encode one UTF-32 codepoint as UTF-8 into buf (must hold >=4 bytes).
 * Returns the byte length, or 0 on invalid input. */
static int sdl2_encode_utf8(uint32_t cp, char* buf) {
    if(cp < 0x80) {
        buf[0] = (char)cp; return 1;
    } else if(cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if(cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if(cp < 0x110000) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* ewebview angles are radians CLOCKWISE from +x (screen y-down). SDL2_gfx's
 * arc/pie take DEGREES measured COUNTERCLOCKWISE from +x on screen (its
 * internal py = y - r*sin(a) flips the sign). Sweep [a0,a1] clockwise in
 * ewebview == sweep [-a1,-a0] counterclockwise in SDL2_gfx, tracing the same
 * pixels; SDL2_gfx requires start <= end (it adds 360 otherwise), which the
 * swap satisfies whenever a1 >= a0. */
static inline Sint16 sdl2_arc_deg(float rad) {
    return (Sint16)(-(double)rad * 180.0 / M_PI);
}

/* ------------------------------------------------------------------ */
/* Graphics / surface                                                  */
/* ------------------------------------------------------------------ */

static eweb_surface_t* ek_surface_new(void* ud, int w, int h) {
    sdl_surf_t* s;
    SDL_Surface* surf;
    SDL_Renderer* rend;
    (void)ud;
    if(w <= 0 || h <= 0) return NULL;
    if(!sdl2_ensure_libs()) return NULL;

    /* Allocate the logical w x h canvas at native resolution: the buffer becomes
     * w*dpr x h*dpr device pixels while the core keeps seeing w x h logical
     * pixels through surface_dims and draws in logical coordinates (scaled back
     * up by every primitive below). */
    {
        int dw = (int)lroundf((float)w * g_dpr);
        int dh = (int)lroundf((float)h * g_dpr);
        if(dw < 1) dw = 1;
        if(dh < 1) dh = 1;
        surf = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_ARGB8888);
    }
    if(!surf) return NULL;
    rend = SDL_CreateSoftwareRenderer(surf);
    if(!rend) { SDL_FreeSurface(surf); return NULL; }
    /* SDL2_gfx blends via SDL_RenderDraw*, which honours the renderer's
     * draw blend mode; BLEND gives us the ARGB alpha-compositing the HAL
     * expects (matching graph_t's premultiplied-ish behaviour closely enough
     * for web content). */
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);

    s = (sdl_surf_t*)calloc(1, sizeof(*s));
    if(!s) { SDL_DestroyRenderer(rend); SDL_FreeSurface(surf); return NULL; }
    s->surf = surf;
    s->rend = rend;
    s->lw = w; s->lh = h; s->dpr = g_dpr;
    return SH(s);
}

static void ek_surface_free(void* ud, eweb_surface_t* h) {
    sdl_surf_t* s;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(s->rend) SDL_DestroyRenderer(s->rend);
    if(s->svg_cache) SDL_FreeSurface(s->svg_cache);
    if(s->svg_doc) svg_doc_free(s->svg_doc);
    if(s->surf) SDL_FreeSurface(s->surf);
    free(s);
}

static void ek_surface_dims(void* ud, eweb_surface_t* h, int* w, int* hh) {
    sdl_surf_t* s;
    (void)ud;
    if(!h) { if(w) *w = 0; if(hh) *hh = 0; return; }
    s = S(h);
    /* LOGICAL pixels: this is the coordinate space the core lays out and draws
     * in (frame-pool revalidation, cache strip heights, image intrinsic size).
     * The device-pixel buffer size lives in surf->w/h and is what
     * surface_native / surface_pixels expose to a DPI-aware embedder. */
    if(w)  *w  = s->lw;
    if(hh) *hh = s->lh;
}

static uint32_t* ek_surface_pixels(void* ud, eweb_surface_t* h, int* w, int* hh) {
    sdl_surf_t* s;
    (void)ud;
    if(!h) { if(w) *w = 0; if(hh) *hh = 0; return NULL; }
    s = S(h);
    if(!s->surf) { if(w) *w = 0; if(hh) *hh = 0; return NULL; }
    /* The renderer writes straight through to surf->pixels (software backend,
     * no batching), so handing out the raw pointer is safe. Callers that
     * mutate it directly should SDL_LockSurface() themselves if the surface
     * happens to be RLE-accelerated; ours never is. */
    if(w)  *w  = s->surf->w;
    if(hh) *hh = s->surf->h;
    return (uint32_t*)s->surf->pixels;
}

/* The concrete native handle: an embedder that IS this port (a desktop SDL2
 * browser shell) can recover the SDL_Surface* and SDL_RenderCopy it straight
 * to the window renderer with no copy. */
static void* ek_surface_native(void* ud, eweb_surface_t* h) {
    (void)ud;
    return h ? (void*)S(h)->surf : NULL;
}

static void ek_surface_clear(void* ud, eweb_surface_t* h, uint32_t color) {
    sdl_surf_t* s;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->surf) return;
    /* SDL_FillRect(NULL rect) fills the whole surface and honours surf->clip_rect,
     * which set_clip keeps in sync with the renderer clip. Color is already
     * 0xAARRGGBB == SDL_PIXELFORMAT_ARGB8888 native order. */
    SDL_FillRect(s->surf, NULL, color);
}

/* set_clip / unset_clip sync BOTH the surface clip (used by SDL_FillRect /
 * SDL_BlitSurface) and the renderer clip (used by every SDL2_gfx primitive),
 * so the two drawing paths agree on the visible region. */
static void ek_surface_set_clip(void* ud, eweb_surface_t* h, int x, int y, int w, int hh) {
    sdl_surf_t* s;
    SDL_Rect r;
    (void)ud;
    if(!h) return;
    s = S(h);
    r.x = sdl2_sp(s, x); r.y = sdl2_sp(s, y); r.w = sdl2_sp(s, w); r.h = sdl2_sp(s, hh);
    if(s->surf) SDL_SetClipRect(s->surf, &r);
    if(s->rend) SDL_RenderSetClipRect(s->rend, &r);
}

static void ek_surface_unset_clip(void* ud, eweb_surface_t* h) {
    sdl_surf_t* s;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(s->surf) SDL_SetClipRect(s->surf, NULL);   /* restores full-surface clip */
    if(s->rend) SDL_RenderSetClipRect(s->rend, NULL);
}

/* ---- rectangles / lines ------------------------------------------- */

static void ek_fill_rect(void* ud, eweb_surface_t* h, int x, int y, int w, int hh, uint32_t color) {
    sdl_surf_t* s;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->surf) return;
    /* The reference graph_fill_cpu OVERWRITES when alpha==0xff and src-over
     * BLENDS per-pixel otherwise. SDL_FillRect never blends - it writes the
     * 0xAARRGGBB word verbatim including the alpha byte - so it is only correct
     * for the opaque case (where it is also by far the fastest path). Route
     * translucent fills through the blending software renderer (boxRGBA, which
     * composites src-over under the renderer's SDL_BLENDMODE_BLEND). */
    if((color >> 24) == 0xFF) {
        SDL_Rect r;
        r.x = sdl2_sp(s, x); r.y = sdl2_sp(s, y); r.w = sdl2_sp(s, w); r.h = sdl2_sp(s, hh);
        SDL_FillRect(s->surf, &r, color);   /* opaque fast path; honours surf clip */
    } else if(s->rend) {
        Uint8 cr, cg, cb, ca;
        int X = sdl2_sp(s, x), Y = sdl2_sp(s, y);
        int W = sdl2_sp(s, w), H = sdl2_sp(s, hh);
        sdl2_unpack_argb(color, &cr, &cg, &cb, &ca);
        boxRGBA(s->rend, (Sint16)X, (Sint16)Y,
                (Sint16)(X + W - 1), (Sint16)(Y + H - 1), cr, cg, cb, ca);
    }
}

static void ek_rect(void* ud, eweb_surface_t* h, int x, int y, int w, int hh, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    /* SDL2_gfx rectangle coords are inclusive corners: (x,y)..(x+w-1,y+h-1). */
    {
        int X = sdl2_sp(s, x), Y = sdl2_sp(s, y);
        int W = sdl2_sp(s, w), H = sdl2_sp(s, hh);
        rectangleRGBA(s->rend, (Sint16)X, (Sint16)Y,
                      (Sint16)(X + W - 1), (Sint16)(Y + H - 1), r, g, b, a);
    }
}

static void ek_line(void* ud, eweb_surface_t* h, int x0, int y0, int x1, int y1, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    lineRGBA(s->rend, (Sint16)sdl2_sp(s, x0), (Sint16)sdl2_sp(s, y0),
             (Sint16)sdl2_sp(s, x1), (Sint16)sdl2_sp(s, y1), r, g, b, a);
}

static void ek_wline(void* ud, eweb_surface_t* h, int x0, int y0, int x1, int y1, int w, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    x0 = sdl2_sp(s, x0); y0 = sdl2_sp(s, y0);
    x1 = sdl2_sp(s, x1); y1 = sdl2_sp(s, y1);
    w  = sdl2_sp(s, w);
    if(w <= 1) {
        lineRGBA(s->rend, (Sint16)x0, (Sint16)y0, (Sint16)x1, (Sint16)y1, r, g, b, a);
    } else {
        /* thickLineRGBA's width parameter is Uint8; clamp to 255. */
        Uint8 ww = (w > 255) ? 255 : (Uint8)w;
        thickLineRGBA(s->rend, (Sint16)x0, (Sint16)y0, (Sint16)x1, (Sint16)y1, ww, r, g, b, a);
    }
}

/* ---- circles / arcs ----------------------------------------------- */

/* SDL2_gfx has no thick-ring circle; emulate by stroking `rw` concentric
 * 1px circles inward from the outer radius. Same trick the EwokOS graph lib
 * uses internally for graph_circle's ring width. */
static void ek_circle(void* ud, eweb_surface_t* h, int x, int y, int radius, int rw, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    int i;
    (void)ud;
    if(!h || radius <= 0) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    if(rw < 1) rw = 1;
    x = sdl2_sp(s, x); y = sdl2_sp(s, y);
    radius = sdl2_sp(s, radius); rw = sdl2_sp(s, rw);
    for(i = 0; i < rw && (radius - i) > 0; i++) {
        circleRGBA(s->rend, (Sint16)x, (Sint16)y, (Sint16)(radius - i), r, g, b, a);
    }
}

static void ek_fill_circle(void* ud, eweb_surface_t* h, int x, int y, int radius, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    (void)ud;
    if(!h || radius <= 0) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    filledCircleRGBA(s->rend, (Sint16)sdl2_sp(s, x), (Sint16)sdl2_sp(s, y),
                     (Sint16)sdl2_sp(s, radius), r, g, b, a);
}

static void ek_arc(void* ud, eweb_surface_t* h, int x, int y, int radius, int rw,
                   float a0, float a1, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    Sint16 start_deg, end_deg;
    int i;
    (void)ud;
    if(!h || radius <= 0) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    start_deg = sdl2_arc_deg(a1);   /* swapped: see sdl2_arc_deg comment */
    end_deg   = sdl2_arc_deg(a0);
    if(rw < 1) rw = 1;
    x = sdl2_sp(s, x); y = sdl2_sp(s, y);
    radius = sdl2_sp(s, radius); rw = sdl2_sp(s, rw);
    for(i = 0; i < rw && (radius - i) > 0; i++) {
        arcRGBA(s->rend, (Sint16)x, (Sint16)y, (Sint16)(radius - i),
                start_deg, end_deg, r, g, b, a);
    }
}

static void ek_fill_arc(void* ud, eweb_surface_t* h, int x, int y, int radius,
                        float a0, float a1, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    Sint16 start_deg, end_deg;
    (void)ud;
    if(!h || radius <= 0) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    start_deg = sdl2_arc_deg(a1);
    end_deg   = sdl2_arc_deg(a0);
    filledPieRGBA(s->rend, (Sint16)sdl2_sp(s, x), (Sint16)sdl2_sp(s, y),
                  (Sint16)sdl2_sp(s, radius), start_deg, end_deg, r, g, b, a);
}

/* ---- rounded rectangles ------------------------------------------- */

static void ek_round(void* ud, eweb_surface_t* h, int x, int y, int w, int hh,
                     int radius, int rw, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    int i;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    if(rw < 1) rw = 1;
    /* Work in device pixels: scale once, then the per-ring inset loop below
     * steps one device pixel per ring as before. */
    x = sdl2_sp(s, x); y = sdl2_sp(s, y);
    w = sdl2_sp(s, w); hh = sdl2_sp(s, hh);
    radius = sdl2_sp(s, radius); rw = sdl2_sp(s, rw);
    /* Shrink the corner radius with each inner ring so the rounded outline
     * stays concentric; clamp at 0 (SDL2_gfx treats rad=0 as a plain rect). */
    for(i = 0; i < rw; i++) {
        int rad = radius - i;
        int x2 = x + w - 1 - i;
        int y2 = y + hh - 1 - i;
        if(x2 <= x + i || y2 <= y + i) break;
        roundedRectangleRGBA(s->rend, (Sint16)(x + i), (Sint16)(y + i),
                             (Sint16)x2, (Sint16)y2,
                             (Sint16)(rad > 0 ? rad : 0), r, g, b, a);
    }
}

static void ek_fill_round(void* ud, eweb_surface_t* h, int x, int y, int w, int hh,
                          int radius, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    {
        int X = sdl2_sp(s, x), Y = sdl2_sp(s, y);
        int W = sdl2_sp(s, w), H = sdl2_sp(s, hh);
        int R = sdl2_sp(s, radius);
        roundedBoxRGBA(s->rend, (Sint16)X, (Sint16)Y,
                       (Sint16)(X + W - 1), (Sint16)(Y + H - 1),
                       (Sint16)(R > 0 ? R : 0), r, g, b, a);
    }
}

/* ---- curves -------------------------------------------------------- */

/* Adaptive de Casteljau subdivision. Emits the polyline into xy[] as
 * interleaved (x,y) floats, EXCLUDING the start point (matches the HAL
 * contract: the caller already knows p0). Tolerance 0.25px is well below the
 * sub-pixel accuracy litehtml/Canvas care about, and depth 12 caps the worst
 * case at 4096 segments. */
#define SDL2_FLATTEN_TOL 0.25f
#define SDL2_FLATTEN_MAX_DEPTH 12

static void sdl2_flatten_quad_rec(float x0, float y0, float cx, float cy, float x1, float y1,
                                   int depth, float* xy, int* n, int max_pts) {
    float mx, my, chord_mx, chord_my, dx, dy;
    float ax, ay, bx, by, midx, midy;
    if(*n >= max_pts) return;
    /* Curve midpoint at t=0.5 vs. chord midpoint: deviation drives subdivision. */
    mx = 0.25f*x0 + 0.5f*cx + 0.25f*x1;
    my = 0.25f*y0 + 0.5f*cy + 0.25f*y1;
    chord_mx = 0.5f*(x0 + x1);
    chord_my = 0.5f*(y0 + y1);
    dx = mx - chord_mx;
    dy = my - chord_my;
    if(depth >= SDL2_FLATTEN_MAX_DEPTH || (dx*dx + dy*dy) <= SDL2_FLATTEN_TOL*SDL2_FLATTEN_TOL) {
        xy[(*n)*2 + 0] = x1;
        xy[(*n)*2 + 1] = y1;
        (*n)++;
        return;
    }
    ax = 0.5f*(x0 + cx); ay = 0.5f*(y0 + cy);
    bx = 0.5f*(cx + x1); by = 0.5f*(cy + y1);
    midx = 0.5f*(ax + bx); midy = 0.5f*(ay + by);
    sdl2_flatten_quad_rec(x0, y0, ax, ay, midx, midy, depth+1, xy, n, max_pts);
    sdl2_flatten_quad_rec(midx, midy, bx, by, x1, y1, depth+1, xy, n, max_pts);
}

static void sdl2_flatten_cubic_rec(float x0, float y0,
                                    float c1x, float c1y, float c2x, float c2y,
                                    float x1, float y1,
                                    int depth, float* xy, int* n, int max_pts) {
    /* Cubic midpoint at t=0.5 vs. chord midpoint. */
    float mx = 0.125f*x0 + 0.375f*c1x + 0.375f*c2x + 0.125f*x1;
    float my = 0.125f*y0 + 0.375f*c1y + 0.375f*c2y + 0.125f*y1;
    float chord_mx = 0.5f*(x0 + x1);
    float chord_my = 0.5f*(y0 + y1);
    float dx = mx - chord_mx;
    float dy = my - chord_my;
    if(*n >= max_pts) return;
    if(depth >= SDL2_FLATTEN_MAX_DEPTH || (dx*dx + dy*dy) <= SDL2_FLATTEN_TOL*SDL2_FLATTEN_TOL) {
        xy[(*n)*2 + 0] = x1;
        xy[(*n)*2 + 1] = y1;
        (*n)++;
        return;
    }
    {
        float a1x = 0.5f*(x0 + c1x),  a1y = 0.5f*(y0 + c1y);
        float a2x = 0.5f*(c1x + c2x), a2y = 0.5f*(c1y + c2y);
        float a3x = 0.5f*(c2x + x1),  a3y = 0.5f*(c2y + y1);
        float b1x = 0.5f*(a1x + a2x), b1y = 0.5f*(a1y + a2y);
        float b2x = 0.5f*(a2x + a3x), b2y = 0.5f*(a2y + a3y);
        float mx0 = 0.5f*(b1x + b2x), my0 = 0.5f*(b1y + b2y);
        sdl2_flatten_cubic_rec(x0, y0, a1x, a1y, b1x, b1y, mx0, my0, depth+1, xy, n, max_pts);
        sdl2_flatten_cubic_rec(mx0, my0, b2x, b2y, a3x, a3y, x1, y1, depth+1, xy, n, max_pts);
    }
}

static int ek_flatten_quadratic(void* ud, float x0, float y0, float cx, float cy,
                                 float x1, float y1, float* xy, int max_pts) {
    int n = 0;
    (void)ud;
    if(!xy || max_pts <= 0) return 0;
    sdl2_flatten_quad_rec(x0, y0, cx, cy, x1, y1, 0, xy, &n, max_pts);
    return n;
}

static int ek_flatten_cubic(void* ud, float x0, float y0,
                             float cx1, float cy1, float cx2, float cy2,
                             float x1, float y1, float* xy, int max_pts) {
    int n = 0;
    (void)ud;
    if(!xy || max_pts <= 0) return 0;
    sdl2_flatten_cubic_rec(x0, y0, cx1, cy1, cx2, cy2, x1, y1, 0, xy, &n, max_pts);
    return n;
}

/* Stroke a quadratic/cubic by flattening then chaining thickLineRGBA between
 * consecutive vertices. SDL2_gfx's bezierRGBA only draws 1px curves, so the
 * flatten+thickline path is the only way to honour the HAL's `w` parameter. */
#define SDL2_STROKE_MAX_PTS 256

static void ek_stroke_quadratic(void* ud, eweb_surface_t* h, int x0, int y0,
                                 int cx, int cy, int x1, int y1, int w, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    float pts[SDL2_STROKE_MAX_PTS * 2];
    int n, i, px, py;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    n = ek_flatten_quadratic(NULL, (float)x0, (float)y0, (float)cx, (float)cy,
                              (float)x1, (float)y1, pts, SDL2_STROKE_MAX_PTS);
    /* pts[] are LOGICAL floats (the core's scanline fill uses the same space);
     * scale each vertex into device pixels only here at draw time. */
    px = sdl2_sp(s, x0); py = sdl2_sp(s, y0);
    w  = sdl2_sp(s, w);
    for(i = 0; i < n; i++) {
        int nx = (int)lroundf(pts[i*2 + 0] * s->dpr);
        int ny = (int)lroundf(pts[i*2 + 1] * s->dpr);
        if(w <= 1) lineRGBA(s->rend, (Sint16)px, (Sint16)py, (Sint16)nx, (Sint16)ny, r, g, b, a);
        else       thickLineRGBA(s->rend, (Sint16)px, (Sint16)py, (Sint16)nx, (Sint16)ny,
                                 (Uint8)(w > 255 ? 255 : w), r, g, b, a);
        px = nx; py = ny;
    }
}

static void ek_stroke_bezier(void* ud, eweb_surface_t* h, int x0, int y0,
                              int cx1, int cy1, int cx2, int cy2, int x1, int y1,
                              int w, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    float pts[SDL2_STROKE_MAX_PTS * 2];
    int n, i, px, py;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    n = ek_flatten_cubic(NULL, (float)x0, (float)y0,
                          (float)cx1, (float)cy1, (float)cx2, (float)cy2,
                          (float)x1, (float)y1, pts, SDL2_STROKE_MAX_PTS);
    /* pts[] are LOGICAL floats; scale each vertex into device pixels here. */
    px = sdl2_sp(s, x0); py = sdl2_sp(s, y0);
    w  = sdl2_sp(s, w);
    for(i = 0; i < n; i++) {
        int nx = (int)lroundf(pts[i*2 + 0] * s->dpr);
        int ny = (int)lroundf(pts[i*2 + 1] * s->dpr);
        if(w <= 1) lineRGBA(s->rend, (Sint16)px, (Sint16)py, (Sint16)nx, (Sint16)ny, r, g, b, a);
        else       thickLineRGBA(s->rend, (Sint16)px, (Sint16)py, (Sint16)nx, (Sint16)ny,
                                 (Uint8)(w > 255 ? 255 : w), r, g, b, a);
        px = nx; py = ny;
    }
}

/* ---- pixel access -------------------------------------------------- */

static void ek_set_pixel(void* ud, eweb_surface_t* h, int x, int y, uint32_t color) {
    sdl_surf_t* s;
    Uint8 r, g, b, a;
    (void)ud;
    if(!h) return;
    s = S(h);
    if(!s->rend) return;
    sdl2_unpack_argb(color, &r, &g, &b, &a);
    pixelRGBA(s->rend, (Sint16)sdl2_sp(s, x), (Sint16)sdl2_sp(s, y), r, g, b, a);
}

static uint32_t ek_get_pixel(void* ud, eweb_surface_t* h, int x, int y) {
    sdl_surf_t* s;
    const uint32_t* px;
    (void)ud;
    if(!h) return 0;
    s = S(h);
    if(!s->surf) return 0;
    x = sdl2_sp(s, x); y = sdl2_sp(s, y);   /* logical -> device; bounds below are device */
    if(x < 0 || y < 0 || x >= s->surf->w || y >= s->surf->h) return 0;
    px = (const uint32_t*)s->surf->pixels;
    /* Surface is always ARGB8888 (we create it that way and convert decoded
     * images to match), so pitch/4 == width and the raw uint32 IS 0xAARRGGBB. */
    return px[(size_t)y * (size_t)(s->surf->pitch / 4) + (size_t)x];
}

/* ---- blit ---------------------------------------------------------- */

/* Copy a libsvg raster (straight-alpha ARGB8888, packed rows) into a fresh
 * SDL surface of the same size; NULL on alloc failure (caller frees img). */
static SDL_Surface* sdl2_surface_from_svg_image(const svg_image_t* img) {
    SDL_Surface* out;
    int y;
    if(!img || img->width <= 0 || img->height <= 0) return NULL;
    out = SDL_CreateRGBSurfaceWithFormat(0, (int)img->width, (int)img->height, 32,
                                         SDL_PIXELFORMAT_ARGB8888);
    if(!out) return NULL;
    for(y = 0; y < (int)img->height; y++)
        memcpy((uint8_t*)out->pixels + (size_t)y * (size_t)out->pitch,
               img->pixels + (size_t)y * img->width,
               (size_t)img->width * sizeof(uint32_t));
    return out;
}

/* Vector counterpart of SDL_BlitScaled for SVG sources: rasterise the whole
 * document at exactly w x h device pixels so the anti-aliased edges land at
 * the destination resolution (bitmap resampling of the intrinsic raster can
 * never be this smooth when shrinking). Memoised per size in src->svg_cache,
 * so repaints/scrolling at an unchanged layout cost a plain 1:1 blit.
 * Returns the cache surface (owned by src), or NULL to fall back. */
static SDL_Surface* sdl2_svg_surface_at(sdl_surf_t* src, int w, int h) {
    svg_image_t* img;
    SDL_Surface* out;
    if(w <= 0 || h <= 0) return NULL;
    if(src->svg_cache && src->svg_cache_w == w && src->svg_cache_h == h)
        return src->svg_cache;
    img = svg_doc_render(src->svg_doc, w, h);
    if(!img) return NULL;
    out = sdl2_surface_from_svg_image(img);
    svg_free(img);
    if(!out) return NULL;
    if(src->svg_cache) SDL_FreeSurface(src->svg_cache);
    src->svg_cache = out;
    src->svg_cache_w = out->w;
    src->svg_cache_h = out->h;
    return out;
}

/* True when the src rect covers the image's whole intrinsic buffer: only
 * then does a re-raster at the dst size represent the same content. */
static bool sdl2_svg_full_src(const sdl_surf_t* src, const SDL_Rect* sr) {
    return src->svg_doc && src->surf &&
           sr->x == 0 && sr->y == 0 &&
           sr->w == src->surf->w && sr->h == src->surf->h;
}

static void ek_blit(void* ud, eweb_surface_t* src_h, int sx, int sy, int sw, int sh,
                    eweb_surface_t* dst_h, int dx, int dy, int dw, int dh) {
    sdl_surf_t *src, *dst;
    SDL_Rect sr, dr;
    (void)ud;
    if(!src_h || !dst_h) return;
    src = S(src_h); dst = S(dst_h);
    if(!src->surf || !dst->surf) return;
    /* Each side lives in its own surface's pixel space: scale the src rect by
     * the source's dpr (decoded images: 1, canvas/frame surfaces: the ratio
     * they were created at) and the dst rect by the destination's dpr. */
    sr.x = sdl2_sp(src, sx); sr.y = sdl2_sp(src, sy); sr.w = sdl2_sp(src, sw); sr.h = sdl2_sp(src, sh);
    dr.x = sdl2_sp(dst, dx); dr.y = sdl2_sp(dst, dy); dr.w = sdl2_sp(dst, dw); dr.h = sdl2_sp(dst, dh);
    /* SVG scaled off its intrinsic size: re-raster the vector at the dst
     * device size instead of resampling the intrinsic bitmap. */
    if(sr.w != dr.w || sr.h != dr.h) {
        if(sdl2_svg_full_src(src, &sr)) {
            SDL_Surface* vec = sdl2_svg_surface_at(src, dr.w, dr.h);
            if(vec) {
                SDL_SetSurfaceBlendMode(vec, SDL_BLENDMODE_NONE);
                SDL_BlitSurface(vec, NULL, dst->surf, &dr);
                return;
            }
        }
    }
    /* The reference graph_blt is a straight COPY, NOT a src-over composite:
     * its fast paths memcpy / copy rows and its resampling paths write the
     * sampled ARGB word verbatim (dst = src, alpha included). So blit must run
     * with BLENDMODE_NONE; only blit_fit_alpha blends. SDL_BlitSurface is the
     * 1:1 native-size copy the HAL documents; fall back to SDL_BlitScaled when
     * the src/dst rects differ in size, matching graph_blt's resample path.
     * dst->clip_rect (kept in sync by set_clip) bounds the write either way. */
    SDL_SetSurfaceBlendMode(src->surf, SDL_BLENDMODE_NONE);
    if(sr.w == dr.w && sr.h == dr.h)
        SDL_BlitSurface(src->surf, &sr, dst->surf, &dr);
    else
        SDL_BlitScaled(src->surf, &sr, dst->surf, &dr);
}

static void ek_blit_fit_alpha(void* ud, eweb_surface_t* src_h, int sx, int sy, int sw, int sh,
                               eweb_surface_t* dst_h, int dx, int dy, int dw, int dh,
                               uint8_t alpha) {
    sdl_surf_t *src, *dst;
    SDL_Rect sr, dr;
    (void)ud;
    if(!src_h || !dst_h) return;
    src = S(src_h); dst = S(dst_h);
    if(!src->surf || !dst->surf) return;
    if(sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    /* src rect in the source's pixel space (decoded images: natural pixels),
     * dst rect in the destination's device pixels. */
    sr.x = sdl2_sp(src, sx); sr.y = sdl2_sp(src, sy); sr.w = sdl2_sp(src, sw); sr.h = sdl2_sp(src, sh);
    dr.x = sdl2_sp(dst, dx); dr.y = sdl2_sp(dst, dy); dr.w = sdl2_sp(dst, dw); dr.h = sdl2_sp(dst, dh);
    /* SVG scaled off its intrinsic size: vector re-raster at the dst device
     * size (the whole point of SVG being resolution-independent), blended
     * src-over like the bitmap path below. */
    if((sr.w != dr.w || sr.h != dr.h) && sdl2_svg_full_src(src, &sr)) {
        SDL_Surface* vec = sdl2_svg_surface_at(src, dr.w, dr.h);
        if(vec) {
            SDL_SetSurfaceBlendMode(vec, SDL_BLENDMODE_BLEND);
            SDL_SetSurfaceAlphaMod(vec, alpha);
            SDL_BlitSurface(vec, NULL, dst->surf, &dr);
            SDL_SetSurfaceAlphaMod(vec, 255);
            return;
        }
    }
    /* SDL_BlitScaled does the src-rect -> dst-rect resample in one call; the
     * per-surface alpha modulation multiplies into every source pixel's own
     * alpha before the blend, which is exactly the HAL's "alpha 0..255"
     * contract. Restore alpha=255 afterwards so the src surface stays usable
     * for subsequent un-modulated blits. */
    SDL_SetSurfaceBlendMode(src->surf, SDL_BLENDMODE_BLEND);
    SDL_SetSurfaceAlphaMod(src->surf, alpha);
    SDL_BlitScaled(src->surf, &sr, dst->surf, &dr);
    SDL_SetSurfaceAlphaMod(src->surf, 255);
}

/* ------------------------------------------------------------------ */
/* Font / text                                                         */
/* ------------------------------------------------------------------ */

/* SDL2_ttf needs a real font FILE; desktop OSes scatter them across a dozen
 * possible paths. Honour EWEBVIEW_SDL2_FONT first (lets the embedder pin a
 * specific face), then walk a short fallback list. The list is CJK-first: the
 * default face must cover Chinese/Japanese/Korean glyphs (UTF-8 pages), so the
 * CJK collections (PingFang / Noto Sans CJK / Microsoft YaHei) come before the
 * Latin-only faces, which stay as last-resort fallbacks. Returns a path that is
 * safe to keep for the process lifetime (either getenv's storage or a string
 * literal). */
static const char* sdl2_find_font_path(const char* family) {
    static const char* const s_paths[] = {
        /* macOS: PingFang SC/TC (full CJK coverage, UTF-8). Present on
         * stock macOS but not on every machine (notably stripped VMs). */
        "/System/Library/Fonts/PingFang.ttc",
        /* macOS: verified-present CJK faces, full Latin coverage too. */
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
        "/System/Library/Fonts/Supplemental/Songti.ttc",
        "/Library/Fonts/Arial Unicode.ttf",
        /* Linux: Noto CJK (covers Chinese/Japanese/Korean). */
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        /* Windows: Microsoft YaHei. */
        "C:\\Windows\\Fonts\\msyh.ttc",
        /* Latin-only last resorts. */
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        NULL
    };
    const char* env;
    int i;
    (void)family;   /* CSS family is advisory; we always resolve to one face. */
    env = SDL_getenv("EWEBVIEW_SDL2_FONT");
    if(env && env[0]) return env;
    for(i = 0; s_paths[i]; i++) {
        SDL_RWops* rw = SDL_RWFromFile(s_paths[i], "rb");
        if(rw) { SDL_RWclose(rw); return s_paths[i]; }
    }
    return NULL;
}

static TTF_Font* sdl_font_face(sdl_font_t* f, int size) {
    const char* path;
    TTF_Font* face;
    int i;
    if(!f) return NULL;
    if(size <= 0) size = 12;
    /* Linear scan: cache is tiny (<=24) and hits are overwhelmingly repeat
     * sizes, so this is faster than any hash we'd write. */
    for(i = 0; i < f->face_count; i++) {
        if(f->faces[i].size == size) return f->faces[i].face;
    }
    if(f->face_count >= SDL_FONT_MAX_FACES) {
        /* Cache full: close slot 0 and reuse it. Safe because the HAL never
         * holds a TTF_Font* across calls - it only sees eweb_font_t. */
        TTF_CloseFont(f->faces[0].face);
        f->faces[0].face = NULL;
        f->faces[0].size = 0;
        /* Compact: shift the rest down so the next miss fills slot 0 cleanly. */
        for(i = 1; i < f->face_count; i++) f->faces[i-1] = f->faces[i];
        f->face_count--;
    }
    path = sdl2_find_font_path(f->family);
    if(!path) return NULL;
    face = TTF_OpenFont(path, size);
    if(!face) return NULL;
    f->faces[f->face_count].size = size;
    f->faces[f->face_count].face = face;
    f->face_count++;
    return face;
}

static eweb_font_t* ek_font_create(void* ud, const char* family) {
    sdl_font_t* f;
    (void)ud;
    if(!sdl2_ensure_libs()) return NULL;
    f = (sdl_font_t*)calloc(1, sizeof(*f));
    if(!f) return NULL;
    if(family) {
        strncpy(f->family, family, sizeof(f->family) - 1);
        f->family[sizeof(f->family) - 1] = 0;
    }
    return FH(f);
}

static void ek_font_destroy(void* ud, eweb_font_t* h) {
    sdl_font_t* f;
    int i;
    (void)ud;
    if(!h) return;
    f = F(h);
    for(i = 0; i < f->face_count; i++) {
        if(f->faces[i].face) TTF_CloseFont(f->faces[i].face);
    }
    free(f);
}

static void ek_font_metrics(void* ud, eweb_font_t* h, int size, eweb_font_metrics_t* out) {
    sdl_font_t* f;
    TTF_Font* face;
    int minx = 0, maxx = 0, miny = 0, maxy = 0, adv = 0;
    (void)ud;
    if(!out) return;
    out->ascent = out->descent = out->height = out->x_height = 0;
    if(!h) return;
    /* font-size:0 (GitHub hides table headers with font-size:0!important):
     * report zero metrics so the line box collapses instead of rasterising
     * at the 12px backstop inside a 0-height row. */
    if(size <= 0) return;
    f = F(h);
    /* Rasterise at device resolution so glyphs are crisp on HiDPI, but report
     * LOGICAL metrics: litehtml lays out with these numbers, so the page keeps
     * its normal CSS-pixel geometry while the pixels come out at native dpi. */
    face = sdl_font_face(f, sdl2_font_px(size));
    if(!face) return;
    out->ascent  = sdl2_logical_px(TTF_FontAscent(face));
    out->descent = sdl2_logical_px(TTF_FontDescent(face));   /* negative below baseline, matches litehtml */
    out->height  = sdl2_logical_px(TTF_FontHeight(face));
    /* x_height: top of the 'x' glyph above the baseline. TTF_GlyphMetrics
     * returns maxy as the distance above baseline, which IS the x-height. */
    if(TTF_GlyphMetrics(face, (Uint16)'x', &minx, &maxx, &miny, &maxy, &adv) == 0) {
        out->x_height = sdl2_logical_px(maxy);
    }
}

static int ek_font_char_width(void* ud, eweb_font_t* h, int size, uint32_t codepoint) {
    sdl_font_t* f;
    TTF_Font* face;
    int minx = 0, maxx = 0, miny = 0, maxy = 0, adv = 0;
    (void)ud;
    if(!h) return 0;
    if(size <= 0) return 0;   /* font-size:0 glyphs advance nothing */
    f = F(h);
    face = sdl_font_face(f, sdl2_font_px(size));   /* device-px face, logical result */
    if(!face) return 0;
    /* BMP fast path: TTF_GlyphMetrics is O(1) (FreeType charmap lookup),
     * which matters because char_width is the hottest layout callback. */
    if(codepoint <= 0xFFFF) {
        if(TTF_GlyphMetrics(face, (Uint16)codepoint, &minx, &maxx, &miny, &maxy, &adv) < 0)
            return 0;
        return sdl2_logical_px(adv);
    }
    /* Supplementary planes: encode as UTF-8 and let TTF_SizeUTF8 walk the
     * (surrogate-paired) glyph. Rare enough that the extra cost is fine. */
    {
        char buf[8];
        int n = sdl2_encode_utf8(codepoint, buf);
        int w = 0, hh = 0;
        if(n <= 0) return 0;
        buf[n] = 0;
        if(TTF_SizeUTF8(face, buf, &w, &hh) < 0) return 0;
        return sdl2_logical_px(w);
    }
}

static void ek_font_text_size(void* ud, eweb_font_t* h, int size, const char* text,
                               int* w, int* hh) {
    sdl_font_t* f;
    TTF_Font* face;
    int ww = 0, hgt = 0;
    (void)ud;
    if(w)  *w  = 0;
    if(hh) *hh = 0;
    if(!h || !text) return;
    if(size <= 0) return;   /* font-size:0 measures empty */
    f = F(h);
    face = sdl_font_face(f, sdl2_font_px(size));
    if(!face) return;
    if(TTF_SizeUTF8(face, text, &ww, &hgt) < 0) return;
    if(w)  *w  = sdl2_logical_px(ww);
    if(hh) *hh = sdl2_logical_px(hgt);
}

static void ek_font_draw_text(void* ud, eweb_surface_t* sh, int x, int y, const char* text,
                               eweb_font_t* fh, int size, uint32_t color) {
    sdl_surf_t* s;
    sdl_font_t* f;
    TTF_Font* face;
    SDL_Color c;
    SDL_Surface* txt;
    SDL_Rect dst;
    (void)ud;
    if(!sh || !fh || !text || !text[0]) return;
    if(size <= 0) return;   /* font-size:0 draws nothing */
    s = S(sh);
    if(!s->surf) return;
    f = F(fh);
    /* Rasterise at the TARGET surface's device resolution and place the glyph
     * bitmap at the surface's device coordinates (s->dpr, not g_dpr: a surface
     * created before a ratio change keeps rendering consistently). */
    face = sdl_font_face(f, sdl2_font_px_dpr(size, s->dpr));
    if(!face) return;
    c.r = (Uint8)((color >> 16) & 0xFF);
    c.g = (Uint8)((color >> 8)  & 0xFF);
    c.b = (Uint8)( color        & 0xFF);
    c.a = (Uint8)((color >> 24) & 0xFF);
    /* TTF_RenderUTF8_Blended produces an ARGB8888 surface with per-pixel
     * alpha; blitting it with BLEND composites correctly onto our target.
     * The HAL anchors text at its TOP-LEFT (matching litehtml's baseline
     * convention via ascent), which is exactly where BlitSurface places it. */
    txt = TTF_RenderUTF8_Blended(face, text, c);
    if(!txt) return;
    SDL_SetSurfaceBlendMode(txt, SDL_BLENDMODE_BLEND);
    dst.x = sdl2_sp(s, x); dst.y = sdl2_sp(s, y); dst.w = txt->w; dst.h = txt->h;
    SDL_BlitSurface(txt, NULL, s->surf, &dst);
    SDL_FreeSurface(txt);
}

/* ------------------------------------------------------------------ */
/* Image decode                                                        */
/* ------------------------------------------------------------------ */

/* SVG is XML text with no binary magic; SDL_image's sniffer misses it unless
 * built with a SVG backend (rare). Detect the <svg root ourselves and let
 * SDL_image try anyway - if it fails, we return NULL and the core degrades
 * to a 0x0 image, matching the HAL contract. */
static bool sdl2_looks_like_svg(const uint8_t* data, int sz) {
    int n = sz < 1024 ? sz : 1024;
    int i;
    for(i = 0; i + 3 < n; i++) {
        if(data[i] == '<' &&
           (data[i+1] == 's' || data[i+1] == 'S') &&
           (data[i+2] == 'v' || data[i+2] == 'V') &&
           (data[i+3] == 'g' || data[i+3] == 'G'))
            return true;
    }
    return false;
}

/* Parse one SVG number ("12", "1.5", "-4"); returns the position after it or
 * NULL if the next token is not a number. Hand-rolled on purpose: strtod()
 * honours the locale decimal separator and SVG always uses '.'. */
static const char* sdl2_svg_num(const char* p, const char* end, double* out) {
    double v = 0.0, frac = 0.0, div = 1.0;
    int neg = 0;
    while(p < end && (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r')) p++;
    if(p < end && (*p == '-' || *p == '+')) { neg = (*p == '-'); p++; }
    if(p >= end || ((*p < '0' || *p > '9') && *p != '.')) return NULL;
    while(p < end && *p >= '0' && *p <= '9') { v = v * 10.0 + (*p - '0'); p++; }
    if(p < end && *p == '.') {
        p++;
        while(p < end && *p >= '0' && *p <= '9') { frac = frac * 10.0 + (*p - '0'); div *= 10.0; p++; }
    }
    *out = neg ? -(v + frac / div) : (v + frac / div);
    return p;
}

/* Intrinsic raster size of an SVG, for IMG_LoadSizedSVG_RW: absolute px
 * width/height attributes win; otherwise the viewBox extent; otherwise 0x0
 * (caller falls back to the unsized load, which lets nanosvg decide). Only the
 * <svg root tag is scanned - width/height/viewBox always live there. */
static void sdl2_svg_natural_size(const uint8_t* data, int sz, int* w, int* h) {
    const char* p = (const char*)data;
    const char* end = p + sz;
    const char* tag;
    const char* tend;
    double dw = 0.0, dh = 0.0, vb[4];
    int have_w = 0, have_h = 0, i;
    *w = 0; *h = 0;
    /* Locate the <svg root tag (same scan as sdl2_looks_like_svg). */
    tag = NULL;
    for(p = (const char*)data; p + 3 < end; p++) {
        if(p[0] == '<' && (p[1] == 's' || p[1] == 'S') &&
           (p[2] == 'v' || p[2] == 'V') && (p[3] == 'g' || p[3] == 'G')) {
            tag = p;
            break;
        }
    }
    if(!tag) return;
    tend = tag;
    while(tend < end && *tend != '>') tend++;
    for(p = tag; p < tend; p++) {
        int namelen;
        const char* vp;
        char quote;
        if(*p != 'w' && *p != 'h' && *p != 'v') continue;
        /* Attribute-name boundary: reject matches inside longer names such as
         * stroke-width / line-height. */
        if(p > tag) {
            char prev = p[-1];
            if((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
               (prev >= '0' && prev <= '9') || prev == '-' || prev == ':' || prev == '_')
                continue;
        }
        if(!strncmp(p, "width", 5)) namelen = 5;
        else if(!strncmp(p, "height", 6)) namelen = 6;
        else if(!strncmp(p, "viewBox", 7) || !strncmp(p, "viewbox", 7)) namelen = 7;
        else continue;
        vp = p + namelen;
        while(vp < tend && (*vp == ' ' || *vp == '\t')) vp++;
        if(vp >= tend || *vp != '=') continue;
        vp++;
        while(vp < tend && (*vp == ' ' || *vp == '\t')) vp++;
        if(vp >= tend || (*vp != '"' && *vp != '\'')) continue;
        quote = *vp++;
        if(namelen == 7) {
            const char* q = vp;
            for(i = 0; i < 4; i++) {
                q = sdl2_svg_num(q, tend, &vb[i]);
                if(!q) break;
            }
            if(i == 4 && vb[2] > 0 && vb[3] > 0) {
                if(!have_w) { dw = vb[2]; have_w = 2; }
                if(!have_h) { dh = vb[3]; have_h = 2; }
            }
        } else {
            double val = 0.0, mult = 1.0;
            const char* q = sdl2_svg_num(vp, tend, &val);
            if(!q || val <= 0) continue;
            /* Physical units convert to CSS px at 96dpi (w3c.svg ships
             * width="5in"); %/em/rem carry no intrinsic size. */
            if(q < tend && *q != quote && *q != ' ' && *q != '\t') {
                if(!strncmp(q, "px", 2))       mult = 1.0;
                else if(!strncmp(q, "in", 2))  mult = 96.0;
                else if(!strncmp(q, "cm", 2))  mult = 96.0 / 2.54;
                else if(!strncmp(q, "mm", 2))  mult = 96.0 / 25.4;
                else if(!strncmp(q, "pt", 2))  mult = 96.0 / 72.0;
                else if(!strncmp(q, "pc", 2))  mult = 16.0;
                else continue;
            }
            val *= mult;
            if(namelen == 5) { dw = val; have_w = 1; }
            else             { dh = val; have_h = 1; }
        }
        /* Resume scanning after the closing quote of this attribute. */
        p = vp;
        while(p < tend && *p != quote) p++;
        if(p >= tend) break;
    }
    if(have_w && have_h) {
        *w = (int)(dw + 0.5);
        *h = (int)(dh + 0.5);
        if(*w > 4096) *w = 4096;
        if(*h > 4096) *h = 4096;
    } else {
        *w = 0; *h = 0;
    }
}

static eweb_surface_t* ek_image_decode(void* ud, const uint8_t* data, int size) {
    SDL_RWops* rw;
    SDL_Surface* img;
    SDL_Surface* argb;
    sdl_surf_t* s;
    (void)ud;
    if(!data || size <= 0) return NULL;
    if(!sdl2_ensure_libs()) return NULL;

    if(sdl2_looks_like_svg(data, size)) {
        /* SVG is vector text: parse it with plutosvg and KEEP the document in
         * the surface handle, so scaled blits re-raster at the exact dst
         * device size (see sdl2_svg_surface_at) - the intrinsic raster below
         * only serves surface_dims (intrinsic CSS size) and 1:1/partial-rect
         * blits. Rasterising once and resampling that bitmap is exactly what
         * makes shrunk SVGs look aliased next to a real browser. */
        svg_doc_t* doc = svg_doc_load(data, (uint32_t)size);
        if(doc) {
            int vw = 0, vh = 0;
            svg_doc_get_size(doc, &vw, &vh);
            svg_image_t* ras = (vw > 0 && vh > 0) ? svg_doc_render(doc, vw, vh) : NULL;
            argb = sdl2_surface_from_svg_image(ras);
            svg_free(ras);
            if(argb) {
                s = (sdl_surf_t*)calloc(1, sizeof(*s));
                if(!s) { SDL_FreeSurface(argb); svg_doc_free(doc); return NULL; }
                s->surf = argb;
                s->rend = NULL;   /* decoded images are blit sources only */
                /* Natural-pixel buffer: intrinsic size == pixel size, no HiDPI
                 * scaling; scaled draws re-raster the vector instead. */
                s->lw = argb->w; s->lh = argb->h; s->dpr = 1.0f;
                s->svg_doc = doc;
                return SH(s);
            }
            svg_doc_free(doc);
        }
        /* plutosvg could not parse/render it: fall back to SDL_image's
         * nanosvg backend. Rasterise at the intrinsic size so nanosvg gets
         * explicit dimensions even for roots that only carry a viewBox or
         * percentage sizes. The sized loader never closes the RWops, so we
         * own it in both outcomes. */
        int vw = 0, vh = 0;
        sdl2_svg_natural_size(data, size, &vw, &vh);
        img = NULL;
        if(vw > 0 && vh > 0) {
            rw = SDL_RWFromConstMem(data, size);
            if(rw) {
                img = IMG_LoadSizedSVG_RW(rw, vw, vh);
                SDL_RWclose(rw);
            }
        }
        if(!img) {
            rw = SDL_RWFromConstMem(data, size);
            if(!rw) return NULL;
            img = IMG_Load_RW(rw, 1);   /* frees rw */
        }
    } else {
        rw = SDL_RWFromConstMem(data, size);
        if(!rw) return NULL;
        img = IMG_Load_RW(rw, 1);   /* frees rw */
    }
    if(!img) return NULL;

    /* Normalise to ARGB8888 so surface_pixels / get_pixel can index the
     * buffer as uint32_t* with the HAL's 0xAARRGGBB layout, and so blits
     * between decoded images and viewport surfaces never trigger a format
     * conversion on the hot path. */
    argb = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(img);
    if(!argb) return NULL;

    /* Decoded images are only ever blit SOURCES (the core never draws into
     * them), so skip the software renderer to save the allocation. The
     * drawing paths NULL-check s->rend before touching it. */
    s = (sdl_surf_t*)calloc(1, sizeof(*s));
    if(!s) { SDL_FreeSurface(argb); return NULL; }
    s->surf = argb;
    s->rend = NULL;
    /* Natural-pixel buffer: intrinsic size == pixel size, no HiDPI scaling. */
    s->lw = argb->w; s->lh = argb->h; s->dpr = 1.0f;
    return SH(s);
}

/* ------------------------------------------------------------------ */
/* Network                                                             */
/* ------------------------------------------------------------------ */

/* Per-request state kept alive between net.request() and net.free_response().
 * Identical to the EwokOS reference port: libtinyhttpsc owns the response
 * buffer and header strings, so we only hold the response handle plus the
 * (key,value) index array we copied out for the HAL. */
typedef struct {
    TinyHttpsResponse* response;
    eweb_http_header_t* headers;
} sdl_http_t;

static bool ek_net_request(void* ud, const char* url,
                           const eweb_http_header_t* req_headers, int req_header_count,
                           eweb_http_response_t* resp) {
    TinyHttpsRequest* request;
    TinyHttpsResponse* response;
    sdl_http_t* st;
    int i, n, body_size;
    const char* body;
    (void)ud;
    if(!url || !resp) return false;
    memset(resp, 0, sizeof(*resp));

    request = NewHttpsRequest(url);
    if(!request) return false;

    HttpsRequestSetTimeout(request, 10000);
    HttpsRequestSetMaxRedirections(request, 0);   /* the core follows redirects */
    HttpsRequestAddHeader(request, "User-Agent", "sdl2-ewebview/1");
    for(i = 0; i < req_header_count; i++) {
        if(req_headers[i].key)
            HttpsRequestAddHeader(request, req_headers[i].key,
                                  req_headers[i].value ? req_headers[i].value : "");
    }

    response = HttpsRequestFetch(request);
    HttpsRequestFree(request);
    if(!response) return false;

    st = (sdl_http_t*)calloc(1, sizeof(sdl_http_t));
    if(!st) { HttpsResponseFree(response); return false; }
    st->response = response;

    /* Copy the header key/value index out into an eweb_http_header_t array.
     * The strings themselves are owned by the response and stay valid until
     * free_response, so only the (key,value) pointer pairs are copied. */
    n = BearHttpsResponse_get_headers_size(response);
    if(n > 0) {
        st->headers = (eweb_http_header_t*)calloc((size_t)n, sizeof(eweb_http_header_t));
        if(st->headers) {
            for(i = 0; i < n; i++) {
                st->headers[i].key   = BearHttpsResponse_get_header_key_by_index(response, i);
                st->headers[i].value = BearHttpsResponse_get_header_value_by_index(response, i);
            }
            resp->headers = st->headers;
            resp->header_count = n;
        }
    }

    body_size = 0;
    body = HttpsResponseReadBody(response, &body_size);
    resp->status    = HttpsResponseGetStatusCode(response);
    resp->error     = HttpsResponseError(response);
    resp->body      = (uint8_t*)body;   /* response-owned; freed with it */
    resp->body_size = (body && body_size > 0) ? body_size : 0;
    resp->native    = st;
    /* The HAL response only carries `error`/`status`, so the core's loadURL
     * failure branches cannot show WHY a transport failed. Surface tinyhttpsc's
     * own error string/code here (same stderr stream as the [ewebview] logs) on
     * a transport error or a non-2xx status, so a failed fetch is diagnosable
     * without a rebuild. */
    if(resp->error || resp->status < 200 || resp->status > 299) {
        fprintf(stderr,
            "[ewebview] net.request diag: url=%s status=%d error=%d code=%d "
            "msg=%s body=%d\n",
            url, resp->status, resp->error ? 1 : 0,
            HttpsResponseGetErrorCode(response),
            HttpsResponseGetErrorMsg(response) ? HttpsResponseGetErrorMsg(response) : "(null)",
            resp->body_size);
    }
    return true;
}

static void ek_net_free_response(void* ud, eweb_http_response_t* resp) {
    sdl_http_t* st;
    (void)ud;
    if(!resp) return;
    st = (sdl_http_t*)resp->native;
    if(st) {
        if(st->headers) free(st->headers);
        if(st->response) HttpsResponseFree(st->response);
        free(st);
    }
    memset(resp, 0, sizeof(*resp));
}

/* file:// reads go through plain C stdio - portable across every desktop OS
 * SDL2 runs on, and free()able by the core with the matching free(). */
static uint8_t* ek_net_read_file(void* ud, const char* path, int* out_size) {
    FILE* fp;
    long sz;
    uint8_t* buf;
    size_t got;
    (void)ud;
    if(out_size) *out_size = 0;
    if(!path || !path[0]) return NULL;
    fp = fopen(path, "rb");
    if(!fp) return NULL;
    if(fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    sz = ftell(fp);
    if(sz < 0) { fclose(fp); return NULL; }
    rewind(fp);
    buf = (uint8_t*)malloc((size_t)sz + 1);   /* +1 so the core can NUL-terminate */
    if(!buf) { fclose(fp); return NULL; }
    got = (sz > 0) ? fread(buf, 1, (size_t)sz, fp) : 0;
    fclose(fp);
    buf[got] = 0;
    if(out_size) *out_size = (int)got;
    return buf;
}

/* "res://" is the port's private resource scheme. The core strips the scheme and
 * hands us the remainder (e.g. "default.css" for "res://default.css"); we resolve
 * it against the directory of the running program, i.e. <program-dir>/res/<name>,
 * mirroring the EwokOS port's x_get_res_name.
 *
 * g_res_base caches "<program-dir>/res/" (trailing slash), computed once in
 * eweb_port_sdl2() from SDL_GetBasePath() on the UI thread before ewebview_create
 * spawns the download worker, so this resolver stays lock-free and re-entrant.
 *
 * The result MUST be absolute: the core dresses it as "file:/" + <result> and
 * loadURL only treats a URL beginning with "file://" as a local read, which needs
 * <result> to start with '/'. SDL_GetBasePath() always returns an absolute path
 * with a trailing separator, so that holds. An absolute remainder is passed
 * through unchanged; if the base path was unavailable we return NULL (scheme
 * unsupported) rather than emit a broken relative path. */
static char g_res_base[1024] = {0};

static const char* ek_net_resolve_resource(void* ud, const char* res, char* buf, int bufsize) {
    int n;
    (void)ud;
    if(!res || !buf || bufsize <= 0) return NULL;
    if(res[0] == '/')
        n = snprintf(buf, (size_t)bufsize, "%s", res);
    else if(g_res_base[0])
        n = snprintf(buf, (size_t)bufsize, "%s%s", g_res_base, res);
    else
        return NULL;   /* base path unavailable; leave res:// unsupported */
    if(n <= 0 || n >= bufsize || buf[0] == 0) return NULL;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

/* The core's chunked style walk (litehtml document::update_master_styles_step)
 * compares an absolute deadline computed here against litehtml's internal
 * sys_tic_ms(), which reads clock_gettime(CLOCK_MONOTONIC). If the port clock
 * used a different epoch (SDL_GetTicks counts from SDL_Init, CLOCK_MONOTONIC
 * from boot), the deadline comparison would be meaningless - sys_tic_ms() would
 * always dwarf the deadline and every chunk would bail immediately, spinning the
 * engine thread at 100% without ever finishing the style pass. Reading the same
 * CLOCK_MONOTONIC keeps the two bases identical. Every other ticMs() use in the
 * core is difference-only, so the epoch choice is irrelevant to them. */
static uint64_t ek_clock_tic_ms(void* ud) {
    (void)ud;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void ek_clock_sleep_ms(void* ud, uint32_t ms) {
    (void)ud;
    SDL_Delay(ms);
}

/* ------------------------------------------------------------------ */
/* Platform utilities                                                  */
/* ------------------------------------------------------------------ */

/* Desktop OSes expose no cheap non-dereferencing heap-membership test (that
 * is an EwokOS libgloss extension). The HAL marks ptr_sane OPTIONAL and the
 * JS<->DOM bridge degrades to trusting the liveness tag alone when it is
 * NULL, so we leave it unset rather than ship a misleading "always true"
 * stub that would defeat the forgery check entirely. */

static void ek_sys_log(void* ud, const char* text) {
    (void)ud;
    if(text) SDL_Log("%s", text);
}

/* System clipboard. SDL hands back an SDL_malloc'd string; copy it into a
 * plain malloc'd buffer so the core can release it with the matching free(). */
static char* ek_sys_clipboard_get(void* ud) {
    (void)ud;
    if(!SDL_HasClipboardText()) return NULL;
    char* sdl = SDL_GetClipboardText();
    if(!sdl) return NULL;
    size_t n = strlen(sdl);
    char* buf = (char*)malloc(n + 1);
    if(buf) memcpy(buf, sdl, n + 1);
    SDL_free(sdl);
    return buf;
}

static void ek_sys_clipboard_set(void* ud, const char* text) {
    (void)ud;
    if(text) SDL_SetClipboardText(text);
}

/* ------------------------------------------------------------------ */
/* Bundle                                                              */
/* ------------------------------------------------------------------ */

/* eweb_port_init() (a plain memset) lives in the ewebview core, not here:
 * it is platform-independent and must exist even when no port is linked. */

void eweb_port_sdl2(eweb_port_t* port, void* ud) {
    if(!port) return;
    memset(port, 0, sizeof(*port));

    /* Cache "<program-dir>/res/" once for the res:// resolver (see
     * ek_net_resolve_resource). Read here on the UI thread - before
     * ewebview_create() spawns the download worker that later calls the resolver
     * - so g_res_base can be used lock-free. Guarded so repeat inits don't
     * re-query SDL_GetBasePath(). */
    if(g_res_base[0] == 0) {
        char* bp = SDL_GetBasePath();
        if(bp) {
            snprintf(g_res_base, sizeof(g_res_base), "%sres/", bp);
            SDL_free(bp);
        }
    }

    port->gfx.ud = ud;
    port->gfx.surface_new       = ek_surface_new;
    port->gfx.surface_free      = ek_surface_free;
    port->gfx.surface_dims      = ek_surface_dims;
    port->gfx.surface_pixels    = ek_surface_pixels;
    port->gfx.surface_native    = ek_surface_native;
    port->gfx.surface_clear     = ek_surface_clear;
    port->gfx.surface_set_clip  = ek_surface_set_clip;
    port->gfx.surface_unset_clip= ek_surface_unset_clip;
    port->gfx.fill_rect         = ek_fill_rect;
    port->gfx.rect              = ek_rect;
    port->gfx.line              = ek_line;
    port->gfx.wline             = ek_wline;
    port->gfx.circle            = ek_circle;
    port->gfx.fill_circle       = ek_fill_circle;
    port->gfx.arc               = ek_arc;
    port->gfx.fill_arc          = ek_fill_arc;
    port->gfx.round             = ek_round;
    port->gfx.fill_round        = ek_fill_round;
    port->gfx.stroke_quadratic  = ek_stroke_quadratic;
    port->gfx.stroke_bezier     = ek_stroke_bezier;
    port->gfx.flatten_quadratic = ek_flatten_quadratic;
    port->gfx.flatten_cubic     = ek_flatten_cubic;
    port->gfx.set_pixel         = ek_set_pixel;
    port->gfx.get_pixel         = ek_get_pixel;
    port->gfx.blit              = ek_blit;
    port->gfx.blit_fit_alpha    = ek_blit_fit_alpha;

    port->font.ud          = ud;
    port->font.create      = ek_font_create;
    port->font.destroy     = ek_font_destroy;
    port->font.metrics     = ek_font_metrics;
    port->font.char_width  = ek_font_char_width;
    port->font.text_size   = ek_font_text_size;
    port->font.draw_text   = ek_font_draw_text;

    port->image.ud     = ud;
    port->image.decode = ek_image_decode;

    port->net.ud               = ud;
    port->net.request          = ek_net_request;
    port->net.free_response    = ek_net_free_response;
    port->net.read_file        = ek_net_read_file;
    port->net.resolve_resource = ek_net_resolve_resource;

    port->clock.ud       = ud;
    port->clock.tic_ms   = ek_clock_tic_ms;
    port->clock.sleep_ms = ek_clock_sleep_ms;

    port->sys.ud       = ud;
    port->sys.ptr_sane = NULL;   /* no portable heap-membership test; OPTIONAL */
    port->sys.log      = ek_sys_log;
    port->sys.clipboard_get = ek_sys_clipboard_get;
    port->sys.clipboard_set = ek_sys_clipboard_set;
}
