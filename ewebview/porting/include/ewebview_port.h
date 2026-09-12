/*
 * ewebview_port.h - the ewebview platform porting layer (HAL).
 *
 * ewebview is a self-contained HTML/CSS/JS web engine. Its core knows nothing
 * about EwokOS, xwin, widget++, graph_t, font_t or tinyhttpsc: it renders a
 * page into an abstract ARGB8888 memory canvas (an eweb_surface_t) and reaches
 * the platform ONLY through the callback tables collected in eweb_port_t.
 *
 * A "port" fills those tables for one platform. The reference EwokOS port
 * (ewebview/porting/src/port_ewokos.c) implements them with the graph/font/
 * tinyhttpsc/kernel_tic primitives; widget++ then merely embeds an ewebview
 * and forwards xwin events into it.
 *
 * ---------------------------------------------------------------------------
 * Design rules
 * ---------------------------------------------------------------------------
 *  - Every table carries its own `ud` (user data) pointer, handed back verbatim
 *    to every callback, so a port can dispatch to its own context. The core
 *    never interprets it.
 *  - eweb_surface_t / eweb_font_t are OPAQUE handles. The core only stores them
 *    and passes them back into the port; the port decides the concrete type
 *    (EwokOS: graph_t* and font_t*). A port that wants to hand the concrete
 *    pointer to the embedder (e.g. so widget++ can graph_blt a frame straight
 *    into its window) exposes it via surface_native().
 *  - Colors are 0xAARRGGBB uint32_t, matching the ARGB8888 pixel layout.
 *  - Coordinates are integers in device pixels unless a prototype says float.
 *  - All callbacks may run on the engine thread or on a download worker thread
 *    (the net + image decode paths deliberately run off the engine thread). The
 *    port must make each table thread-safe to the extent the core calls it from
 *    more than one thread; the drawing/font tables are engine-thread only, the
 *    net table is worker-thread only, the clock is called from both.
 *  - Every callback is OPTIONAL unless marked REQUIRED: a NULL hook makes the
 *    matching capability degrade instead of crashing, so a port can be built up
 *    incrementally (e.g. gfx + font first, then net, then image).
 */

#ifndef EWEBVIEW_PORT_H
#define EWEBVIEW_PORT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Opaque platform handles                                             */
/* ------------------------------------------------------------------ */

/* An ARGB8888 memory canvas. EwokOS port: a graph_t*. */
typedef struct eweb_surface eweb_surface_t;

/* A font face handle. EwokOS port: a font_t* (size is passed per call). */
typedef struct eweb_font    eweb_font_t;

/* ------------------------------------------------------------------ */
/* Graphics / surface table                                            */
/* ------------------------------------------------------------------ */

/* Font metrics, filled by eweb_font_api_t::metrics. Values are in pixels,
 * mirroring litehtml's font_metrics (ascent/descent/height/x_height). */
typedef struct eweb_font_metrics {
    int ascent;
    int descent;
    int height;
    int x_height;
} eweb_font_metrics_t;

typedef struct eweb_gfx {
    void* ud;

    /* ---- surface lifecycle (REQUIRED: surface_new, surface_free) ---- */

    /* Allocate a w x h ARGB8888 canvas owned by the port. NULL on failure.
     * The core uses this for the viewport frame pool, for <canvas> backing
     * stores and for anonymous offscreen bitmaps. */
    eweb_surface_t* (*surface_new)(void* ud, int w, int h);

    /* Free a surface created by surface_new / image.decode. Must tolerate NULL. */
    void (*surface_free)(void* ud, eweb_surface_t* s);

    /* Query the pixel dimensions. Either out pointer may be NULL. REQUIRED
     * (the core's frame pool revalidates recycled buffers against the current
     * viewport size before reusing them). */
    void (*surface_dims)(void* ud, eweb_surface_t* s, int* w, int* h);

    /* Direct ARGB8888 pixel access (w*h uint32_t). May return NULL if the port
     * cannot expose the buffer; the core then falls back to get/set_pixel and
     * the blit primitives. Used by Canvas ImageData read/write. */
    uint32_t* (*surface_pixels)(void* ud, eweb_surface_t* s, int* w, int* h);

    /* Return the port's concrete native handle for a surface (EwokOS: the
     * graph_t*), so an embedder that IS the port can blit a frame straight into
     * its own window surface. May be NULL. The core never dereferences it. */
    void* (*surface_native)(void* ud, eweb_surface_t* s);

    /* Fill the whole surface with `color` (honours the current clip). */
    void (*surface_clear)(void* ud, eweb_surface_t* s, uint32_t color);

    /* ---- clipping (rectangular; OPTIONAL) ---- */
    void (*surface_set_clip)(void* ud, eweb_surface_t* s, int x, int y, int w, int h);
    void (*surface_unset_clip)(void* ud, eweb_surface_t* s);

    /* ---- rectangles / lines (color 0xAARRGGBB) ---- */
    void (*fill_rect)(void* ud, eweb_surface_t* s, int x, int y, int w, int h, uint32_t color);
    void (*rect)(void* ud, eweb_surface_t* s, int x, int y, int w, int h, uint32_t color);
    void (*line)(void* ud, eweb_surface_t* s, int x0, int y0, int x1, int y1, uint32_t color);
    void (*wline)(void* ud, eweb_surface_t* s, int x0, int y0, int x1, int y1, int w, uint32_t color);

    /* ---- circles / arcs (angles in radians, clockwise from +x) ---- */
    void (*circle)(void* ud, eweb_surface_t* s, int x, int y, int radius, int rw, uint32_t color);
    void (*fill_circle)(void* ud, eweb_surface_t* s, int x, int y, int radius, uint32_t color);
    void (*arc)(void* ud, eweb_surface_t* s, int x, int y, int radius, int rw,
                float start_angle, float end_angle, uint32_t color);
    void (*fill_arc)(void* ud, eweb_surface_t* s, int x, int y, int radius,
                     float start_angle, float end_angle, uint32_t color);

    /* ---- rounded rectangles ---- */
    void (*round)(void* ud, eweb_surface_t* s, int x, int y, int w, int h, int radius, int rw, uint32_t color);
    void (*fill_round)(void* ud, eweb_surface_t* s, int x, int y, int w, int h, int radius, uint32_t color);

    /* ---- curves (device-space ints for the strokers; floats for flatten) ----
     * flatten_* write the flattened polyline into `xy` as interleaved x,y
     * floats (up to max_pts vertices, start point excluded) and return the
     * vertex count. They let the core scanline-FILL a curve without
     * re-implementing the port's subdivision math. OPTIONAL: if NULL, the core
     * approximates the flatten with straight chords. */
    void (*stroke_quadratic)(void* ud, eweb_surface_t* s, int x0, int y0,
                             int cx, int cy, int x1, int y1, int w, uint32_t color);
    void (*stroke_bezier)(void* ud, eweb_surface_t* s, int x0, int y0,
                          int cx1, int cy1, int cx2, int cy2, int x1, int y1,
                          int w, uint32_t color);
    int  (*flatten_quadratic)(void* ud, float x0, float y0, float cx, float cy,
                              float x1, float y1, float* xy, int max_pts);
    int  (*flatten_cubic)(void* ud, float x0, float y0, float cx1, float cy1,
                          float cx2, float cy2, float x1, float y1,
                          float* xy, int max_pts);

    /* ---- pixel access ---- */
    void     (*set_pixel)(void* ud, eweb_surface_t* s, int x, int y, uint32_t color);
    uint32_t (*get_pixel)(void* ud, eweb_surface_t* s, int x, int y);

    /* ---- blit (drawImage / patterns / frame composite) ----
     * blit copies 1:1 (src rect -> dst rect at native size). blit_fit_alpha
     * scales the src rect into the dst rect and blends with `alpha` (0..255).
     * REQUIRED: blit_fit_alpha (background images) and blit (canvas composite). */
    void (*blit)(void* ud, eweb_surface_t* src, int sx, int sy, int sw, int sh,
                 eweb_surface_t* dst, int dx, int dy, int dw, int dh);
    void (*blit_fit_alpha)(void* ud, eweb_surface_t* src, int sx, int sy, int sw, int sh,
                           eweb_surface_t* dst, int dx, int dy, int dw, int dh, uint8_t alpha);
} eweb_gfx_t;

/* ------------------------------------------------------------------ */
/* Font / text table                                                   */
/* ------------------------------------------------------------------ */

typedef struct eweb_font_api {
    void* ud;

    /* Create a font handle for `family` (e.g. "sans-serif"). The port maps the
     * CSS family onto a concrete face (EwokOS: "system-cn"). Size is NOT baked
     * into the handle - it is passed to every measurement/draw call below, so
     * one handle serves all sizes. NULL on failure. REQUIRED. */
    eweb_font_t* (*create)(void* ud, const char* family);

    /* Destroy a font handle. Must tolerate NULL. REQUIRED. */
    void (*destroy)(void* ud, eweb_font_t* f);

    /* Fill pixel metrics for `f` rendered at `size`. REQUIRED (litehtml needs
     * ascent/descent/height/x_height to lay out text). */
    void (*metrics)(void* ud, eweb_font_t* f, int size, eweb_font_metrics_t* out);

    /* Advance width of one UTF-32 codepoint at `size`, in pixels. REQUIRED
     * (the hottest callback of every litehtml layout pass). */
    int (*char_width)(void* ud, eweb_font_t* f, int size, uint32_t codepoint);

    /* Total size of a UTF-8 string at `size`. Either out pointer may be NULL.
     * OPTIONAL: the core falls back to summing char_width per codepoint. */
    void (*text_size)(void* ud, eweb_font_t* f, int size, const char* text, int* w, int* h);

    /* Draw a UTF-8 string with its top-left at (x,y) onto surface `s`. REQUIRED. */
    void (*draw_text)(void* ud, eweb_surface_t* s, int x, int y, const char* text,
                      eweb_font_t* f, int size, uint32_t color);
} eweb_font_api_t;

/* ------------------------------------------------------------------ */
/* Image decode table                                                  */
/* ------------------------------------------------------------------ */

typedef struct eweb_image_api {
    void* ud;

    /* Decode raw image bytes (png/jpeg/gif/svg/webp/tga/...) into a NEW
     * ARGB8888 surface. Returns NULL if the format is unsupported or the data
     * is corrupt. The core frees the result with gfx.surface_free. This runs on
     * a download worker thread (pure heap work), so it must not touch any
     * engine-thread-only state. REQUIRED for <img>/background-image support. */
    eweb_surface_t* (*decode)(void* ud, const uint8_t* data, int size);
} eweb_image_api_t;

/* ------------------------------------------------------------------ */
/* Network / resource table                                            */
/* ------------------------------------------------------------------ */

/* One HTTP header (request or response). Both strings are borrowed; the port
 * copies anything it needs to keep. */
typedef struct eweb_http_header {
    const char* key;
    const char* value;
} eweb_http_header_t;

/* The result of ONE http(s) request. `body` and `headers` are owned by the
 * port and stay valid until the core calls net.free_response(resp). */
typedef struct eweb_http_response {
    int  status;                 /* HTTP status code (0 when transport failed) */
    bool error;                  /* true on a transport/TLS error */
    uint8_t* body;               /* response body (not NUL-terminated), or NULL */
    int  body_size;              /* length of body in bytes */
    const eweb_http_header_t* headers;  /* response headers, or NULL */
    int  header_count;
    void* native;                /* port-private (e.g. the client response obj) */
} eweb_http_response_t;

typedef struct eweb_net_api {
    void* ud;

    /* Perform ONE http(s) request WITHOUT following redirects (the core follows
     * redirects itself so it can re-scope cookies per hop). `req_headers`
     * carries the Cookie:/User-Agent: headers the core built for this hop.
     * Returns true and fills *resp when a response was received (even 4xx/5xx);
     * returns false on a transport failure. Runs on a download worker thread.
     * REQUIRED for http(s) pages. */
    bool (*request)(void* ud, const char* url,
                    const eweb_http_header_t* req_headers, int req_header_count,
                    eweb_http_response_t* resp);

    /* Release everything `resp` owns (body, headers, native client objects).
     * Called once per successful request(). REQUIRED alongside request(). */
    void (*free_response)(void* ud, eweb_http_response_t* resp);

    /* Read a local file by absolute path (the file:// scheme). Returns a
     * malloc'd buffer (the core free()s it) and sets *out_size, or NULL.
     * REQUIRED for file:// pages. */
    uint8_t* (*read_file)(void* ud, const char* path, int* out_size);

    /* Resolve a resource reference (the port's private scheme, e.g. EwokOS
     * "res://") to a real filesystem path, written into `buf` (<= bufsize).
     * Returns buf, or NULL when the scheme is unknown/unsupported. OPTIONAL. */
    const char* (*resolve_resource)(void* ud, const char* res, char* buf, int bufsize);
} eweb_net_api_t;

/* ------------------------------------------------------------------ */
/* Clock table                                                         */
/* ------------------------------------------------------------------ */

typedef struct eweb_clock_api {
    void* ud;

    /* Monotonic millisecond clock. Only differences are meaningful. Used to
     * drive CSS/JS timers, debounce layout, and budget VM runs. Called from
     * both the engine and worker threads. REQUIRED. */
    uint64_t (*tic_ms)(void* ud);

    /* Sleep the calling thread for `ms` milliseconds. Used by the engine when
     * it must wait for a download worker to notice a cancellation. OPTIONAL:
     * the core falls back to a pthread condition-variable timed wait. */
    void (*sleep_ms)(void* ud, uint32_t ms);
} eweb_clock_api_t;

/* ------------------------------------------------------------------ */
/* Platform utilities table                                            */
/* ------------------------------------------------------------------ */

typedef struct eweb_sys_api {
    void* ud;

    /* Cheap, NON-dereferencing pointer-plausibility test: true when `p` points
     * inside the process's live heap. The JS<->DOM bridge uses it to reject a
     * forged element handle BEFORE reading its liveness tag through it
     * (EwokOS: ewok_ptr_in_heap). OPTIONAL: without it the bridge trusts the
     * liveness tag alone. */
    bool (*ptr_sane)(void* ud, const void* p);

    /* Log one line of text from the core (JS console output, engine warnings).
     * The text is already prefixed with its source tag (e.g. "[js]"). Runs on
     * the engine thread. OPTIONAL: without it the core drops log output. */
    void (*log)(void* ud, const char* text);
} eweb_sys_api_t;

/* ------------------------------------------------------------------ */
/* The complete port                                                   */
/* ------------------------------------------------------------------ */

/* Bundle of every platform table ewebview needs. Copied by value at
 * ewebview_create(), so the caller may free its copy afterwards. A table left
 * entirely zeroed disables the matching capability (see each table's REQUIRED
 * notes). gfx + font + clock are the minimum for a page to lay out and paint. */
typedef struct eweb_port {
    eweb_gfx_t         gfx;
    eweb_font_api_t    font;
    eweb_image_api_t   image;
    eweb_net_api_t     net;
    eweb_clock_api_t   clock;
    eweb_sys_api_t     sys;
} eweb_port_t;

/* Zero an eweb_port_t. Handy for ports that fill only a few tables. */
void eweb_port_init(eweb_port_t* port);

/* ------------------------------------------------------------------ */
/* Reference EwokOS port                                               */
/* ------------------------------------------------------------------ */

/* Fill `port` with the EwokOS implementation: surfaces are graph_t*, fonts are
 * font_t*, images decode via graph_image/libwebp/plutosvg, network uses
 * tinyhttpsc + vfs_readfile, resources resolve via libx x_get_res_name, and the
 * clock is kernel_tic_ms. `ud` is stored in every table's ud and may be NULL
 * (the EwokOS port keeps no per-instance state). Declared here so an embedder
 * can opt into the reference port without knowing its internals. */
void eweb_port_ewokos(eweb_port_t* port, void* ud);

#ifdef __cplusplus
}
#endif

#endif /* EWEBVIEW_PORT_H */
