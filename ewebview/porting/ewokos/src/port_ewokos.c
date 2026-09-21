/*
 * port_ewokos.c - the reference EwokOS implementation of the ewebview HAL.
 *
 * Maps every ewebview porting table onto EwokOS primitives:
 *   eweb_surface_t  -> graph_t*      (ARGB8888, system/gui/libs/graph)
 *   eweb_font_t     -> font_t*       (system/gui/libs/font, FreeType backed)
 *   image.decode    -> plutosvg / libwebp / graph_image (png/jpeg/gif/tga/svg)
 *   net.request     -> libtinyhttpsc (BearSSL HTTP/HTTPS), one hop per call
 *   net.read_file   -> vfs_readfile  (file://)
 *   net.resolve_res -> x_get_res_name (res://)
 *   clock.tic_ms    -> kernel_tic_ms
 *
 * The two opaque handles are the concrete EwokOS pointers plain-cast, so an
 * embedder that IS this port (widget++) can recover the graph_t* of a frame
 * with a cast and graph_blt it straight into its window with no copy.
 *
 * Threading: gfx/font run on the engine thread only; net + image.decode run on
 * a download worker thread; clock runs on both. None of the EwokOS primitives
 * below keep cross-call state, so each table is re-entrant across threads.
 *
 * Plain C99: the port needs no C++ (the res:// resolver uses libx's
 * x_get_res_name, not x++'s X::getResFullName), so it builds with $(CC).
 */

#include <ewebview_port.h>

#include <graph/graph.h>
#include <graph/graph_ex.h>
#include <graph/graph_image.h>
#include <graph/curve.h>
#include <font/font.h>
#include <webp.h>
#include <ewoksys/vfs.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <tinyhttpsc/tinyhttpsc.h>
#include <tinyhttpsc/BearHttpsClientOne.h>
#include <x/x.h>
#include <clipboard/clipboard.h>
#include <zlib.h>   /* gzip/deflate Content-Encoding decode (ek_inflate_body) */

#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <math.h>      /* lroundf: logical<->device pixel rounding */

/* ------------------------------------------------------------------ */
/* Handle <-> concrete type                                            */
/* ------------------------------------------------------------------ */

/* HiDPI mechanism: an eweb_surface_t handle is an ek_surf_t wrapping a
 * DEVICE-pixel graph_t (g->w/h == lw*dpr x lh*dpr). The core lays out and
 * draws in LOGICAL (CSS) pixels - surface_dims reports lw/lh - and every
 * draw/font callback scales the incoming logical coordinates up by the target
 * surface's dpr, so the page can rasterise at native resolution while layout,
 * media queries and srcset selection stay in CSS pixels. Decoded images are
 * natural-pixel buffers with dpr == 1; blit scales the src rect by the
 * source's dpr and the dst rect by the destination's.
 *
 * On EwokOS the embedder sets dpr = 1 by default (see WidgetWebview), so every
 * path below is numerically identical to a plain 1x port - no scaling. The
 * machinery only kicks in when a caller opts into a higher ratio. */
typedef struct {
    graph_t* g;    /* device-pixel buffer: w = lw*dpr, h = lh*dpr        */
    int      lw;   /* logical (CSS) width the core sees via surface_dims */
    int      lh;   /* logical (CSS) height                               */
    float    dpr;  /* device px per logical px (decoded images: 1)       */
} ek_surf_t;

/* Device pixels per logical (CSS) pixel, set by the embedder via
 * eweb_port_ewokos_set_dpr() BEFORE the first ewebview_set_viewport so the
 * frame pool allocates at the right device size. 1.0 == plain 1x port. */
static float g_dpr = 1.0f;

#define S(h)  ((ek_surf_t*)(h))
#define SH(s) ((eweb_surface_t*)(s))
#define G(h)  (S(h)->g)
#define FT(f) ((font_t*)(f))
#define EF(f) ((eweb_font_t*)(f))

/* Scale a logical coordinate/length into a surface's device pixels. */
#define X(h, v) ((int)lroundf((float)(v) * S(h)->dpr))

/* Wrap a freshly created/decoded device graph_t in a surface handle. The
 * caller-visible (logical) size equals the buffer size scaled back by dpr. */
static eweb_surface_t* ek_wrap(graph_t* g, float dpr) {
    ek_surf_t* s;
    if(!g) return NULL;
    s = (ek_surf_t*)calloc(1, sizeof(*s));
    if(!s) { graph_free(g); return NULL; }
    s->g = g;
    s->dpr = (dpr > 0.0f) ? dpr : 1.0f;
    s->lw = (int)lroundf((float)g->w / s->dpr);
    s->lh = (int)lroundf((float)g->h / s->dpr);
    if(s->lw <= 0) s->lw = g->w;
    if(s->lh <= 0) s->lh = g->h;
    return SH(s);
}

/* ------------------------------------------------------------------ */
/* Graphics / surface                                                  */
/* ------------------------------------------------------------------ */

static eweb_surface_t* ek_surface_new(void* ud, int w, int h) {
    ek_surf_t* s;
    int dw, dh;
    (void)ud;
    if(w <= 0 || h <= 0) return NULL;
    dw = (int)lroundf((float)w * g_dpr);
    dh = (int)lroundf((float)h * g_dpr);
    if(dw <= 0) dw = 1;
    if(dh <= 0) dh = 1;
    s = (ek_surf_t*)calloc(1, sizeof(*s));
    if(!s) return NULL;
    s->g = graph_new(NULL, dw, dh);
    if(!s->g) { free(s); return NULL; }
    s->lw = w; s->lh = h; s->dpr = g_dpr;
    return SH(s);
}
static void ek_surface_free(void* ud, eweb_surface_t* s) {
    (void)ud;
    if(!s) return;
    if(G(s)) graph_free(G(s));
    free(S(s));
}
static void ek_surface_dims(void* ud, eweb_surface_t* s, int* w, int* h) {
    (void)ud;
    /* LOGICAL (CSS) size: the core lays out in these units. */
    if(w) *w = s ? S(s)->lw : 0;
    if(h) *h = s ? S(s)->lh : 0;
}
static uint32_t* ek_surface_pixels(void* ud, eweb_surface_t* s, int* w, int* h) {
    graph_t* g;
    (void)ud;
    g = s ? G(s) : NULL;
    if(!g) { if(w) *w = 0; if(h) *h = 0; return NULL; }
    /* DEVICE pixels: raw buffer for getImageData/putImageData + rounded masks. */
    if(w) *w = g->w;
    if(h) *h = g->h;
    return g->buffer;
}
static void* ek_surface_native(void* ud, eweb_surface_t* s) { (void)ud; return s ? (void*)G(s) : NULL; }
static void ek_surface_clear(void* ud, eweb_surface_t* s, uint32_t c) { (void)ud; if(s) graph_clear(G(s), c); }
static void ek_surface_set_clip(void* ud, eweb_surface_t* s, int x, int y, int w, int h) { (void)ud; if(s) graph_set_clip(G(s), X(s,x), X(s,y), X(s,w), X(s,h)); }
static void ek_surface_unset_clip(void* ud, eweb_surface_t* s) { (void)ud; if(s) graph_unset_clip(G(s)); }

static void ek_fill_rect(void* ud, eweb_surface_t* s, int x, int y, int w, int h, uint32_t c) { (void)ud; if(s) graph_fill_rect(G(s), X(s,x), X(s,y), X(s,w), X(s,h), c); }
static void ek_rect(void* ud, eweb_surface_t* s, int x, int y, int w, int h, uint32_t c) { (void)ud; if(s) graph_rect(G(s), X(s,x), X(s,y), X(s,w), X(s,h), c); }
static void ek_line(void* ud, eweb_surface_t* s, int x0, int y0, int x1, int y1, uint32_t c) { (void)ud; if(s) graph_line(G(s), X(s,x0), X(s,y0), X(s,x1), X(s,y1), c); }
static void ek_wline(void* ud, eweb_surface_t* s, int x0, int y0, int x1, int y1, int w, uint32_t c) { (void)ud; if(s) graph_wline(G(s), X(s,x0), X(s,y0), X(s,x1), X(s,y1), (uint32_t)X(s,w), c); }

static void ek_circle(void* ud, eweb_surface_t* s, int x, int y, int r, int rw, uint32_t c) { (void)ud; if(s) graph_circle(G(s), X(s,x), X(s,y), X(s,r), X(s,rw), c); }
static void ek_fill_circle(void* ud, eweb_surface_t* s, int x, int y, int r, uint32_t c) { (void)ud; if(s) graph_fill_circle(G(s), X(s,x), X(s,y), X(s,r), c); }
static void ek_arc(void* ud, eweb_surface_t* s, int x, int y, int r, int rw, float a0, float a1, uint32_t c) { (void)ud; if(s) graph_arc(G(s), X(s,x), X(s,y), X(s,r), X(s,rw), a0, a1, c); }
static void ek_fill_arc(void* ud, eweb_surface_t* s, int x, int y, int r, float a0, float a1, uint32_t c) { (void)ud; if(s) graph_fill_arc(G(s), X(s,x), X(s,y), X(s,r), a0, a1, c); }

static void ek_round(void* ud, eweb_surface_t* s, int x, int y, int w, int h, int r, int rw, uint32_t c) { (void)ud; if(s) graph_round(G(s), X(s,x), X(s,y), X(s,w), X(s,h), X(s,r), X(s,rw), c); }
static void ek_fill_round(void* ud, eweb_surface_t* s, int x, int y, int w, int h, int r, uint32_t c) { (void)ud; if(s) graph_fill_round(G(s), X(s,x), X(s,y), X(s,w), X(s,h), X(s,r), c); }

static void ek_stroke_quadratic(void* ud, eweb_surface_t* s, int x0, int y0, int cx, int cy, int x1, int y1, int w, uint32_t c) {
    (void)ud; if(s) graph_quadratic_curve_w(G(s), X(s,x0), X(s,y0), X(s,cx), X(s,cy), X(s,x1), X(s,y1), X(s,w), c);
}
static void ek_stroke_bezier(void* ud, eweb_surface_t* s, int x0, int y0, int cx1, int cy1, int cx2, int cy2, int x1, int y1, int w, uint32_t c) {
    (void)ud; if(s) graph_bezier_curve_w(G(s), X(s,x0), X(s,y0), X(s,cx1), X(s,cy1), X(s,cx2), X(s,cy2), X(s,x1), X(s,y1), X(s,w), c);
}
/* Pure geometry (no surface): the core rasterises the returned points through
 * the scaled primitives above, so flattening stays in logical space. */
static int ek_flatten_quadratic(void* ud, float x0, float y0, float cx, float cy, float x1, float y1, float* xy, int max_pts) {
    (void)ud; return graph_flatten_quadratic(x0, y0, cx, cy, x1, y1, xy, max_pts);
}
static int ek_flatten_cubic(void* ud, float x0, float y0, float cx1, float cy1, float cx2, float cy2, float x1, float y1, float* xy, int max_pts) {
    (void)ud; return graph_flatten_cubic(x0, y0, cx1, cy1, cx2, cy2, x1, y1, xy, max_pts);
}

static void ek_set_pixel(void* ud, eweb_surface_t* s, int x, int y, uint32_t c) { (void)ud; if(s) graph_pixel(G(s), X(s,x), X(s,y), c); }
static uint32_t ek_get_pixel(void* ud, eweb_surface_t* s, int x, int y) { (void)ud; return s ? graph_get_pixel(G(s), X(s,x), X(s,y)) : 0; }

static void ek_blit(void* ud, eweb_surface_t* src, int sx, int sy, int sw, int sh,
                    eweb_surface_t* dst, int dx, int dy, int dw, int dh) {
    int SX, SY, SW, SH, DX, DY, DW, DH;
    (void)ud;
    if(!src || !dst) return;
    /* Source rect in the source's device px, destination rect in the dst's
     * device px. When an image (dpr==1) is drawn into a HiDPI frame (dpr==N)
     * only the dst side scales, so the source is not truncated to the CSS box.
     * graph_blt() is a 1:1 copy; anything scaled goes through graph_blt_fit(). */
    SX = (int)lroundf((float)sx * S(src)->dpr); SY = (int)lroundf((float)sy * S(src)->dpr);
    SW = (int)lroundf((float)sw * S(src)->dpr); SH = (int)lroundf((float)sh * S(src)->dpr);
    DX = (int)lroundf((float)dx * S(dst)->dpr); DY = (int)lroundf((float)dy * S(dst)->dpr);
    DW = (int)lroundf((float)dw * S(dst)->dpr); DH = (int)lroundf((float)dh * S(dst)->dpr);
    if(SW == DW && SH == DH)
        graph_blt(G(src), SX, SY, SW, SH, G(dst), DX, DY, DW, DH);
    else
        graph_blt_fit(G(src), SX, SY, SW, SH, G(dst), DX, DY, DW, DH);
}
static void ek_blit_fit_alpha(void* ud, eweb_surface_t* src, int sx, int sy, int sw, int sh,
                              eweb_surface_t* dst, int dx, int dy, int dw, int dh, uint8_t alpha) {
    int SX, SY, SW, SH, DX, DY, DW, DH;
    (void)ud;
    if(!src || !dst) return;
    SX = (int)lroundf((float)sx * S(src)->dpr); SY = (int)lroundf((float)sy * S(src)->dpr);
    SW = (int)lroundf((float)sw * S(src)->dpr); SH = (int)lroundf((float)sh * S(src)->dpr);
    DX = (int)lroundf((float)dx * S(dst)->dpr); DY = (int)lroundf((float)dy * S(dst)->dpr);
    DW = (int)lroundf((float)dw * S(dst)->dpr); DH = (int)lroundf((float)dh * S(dst)->dpr);
    graph_blt_fit_alpha(G(src), SX, SY, SW, SH, G(dst), DX, DY, DW, DH, alpha);
}

/* ------------------------------------------------------------------ */
/* Font / text                                                         */
/* ------------------------------------------------------------------ */

static eweb_font_t* ek_font_create(void* ud, const char* family) {
    (void)ud; (void)family;
    /* EwokOS ships one CJK-capable system face; the CSS family is advisory. */
    return EF(font_new("system-cn", false));
}
static void ek_font_destroy(void* ud, eweb_font_t* f) { (void)ud; if(f) font_free(FT(f)); }

static void ek_font_metrics(void* ud, eweb_font_t* f, int size, eweb_font_metrics_t* out) {
    int dsize;
    (void)ud;
    if(!out) return;
    out->ascent = out->descent = out->height = out->x_height = 0;
    if(!f) return;
    /* Measure at DEVICE size (size*dpr) and report LOGICAL metrics (/dpr), so
     * the layout units match the glyphs draw_text() rasterises at device size. */
    dsize = (int)lroundf((float)size * g_dpr);
    if(dsize <= 0) dsize = size;
    face_info_t face;
    if(font_get_face(FT(f), (uint32_t)dsize, &face) == 0) {
        const int DENT = 64;   /* FreeType 26.6 fixed point */
        out->ascent  = (int)lroundf((float)(face.ascender / DENT) / g_dpr);
        out->descent = (int)lroundf((float)(face.descender / DENT) / g_dpr);
        out->height  = (int)lroundf((float)(face.height / DENT) / g_dpr);
    }
    uint32_t xh = 0;
    font_char_size('x', FT(f), (uint32_t)dsize, &xh, NULL);
    out->x_height = (int)lroundf((float)xh / g_dpr);
}
static int ek_font_char_width(void* ud, eweb_font_t* f, int size, uint32_t codepoint) {
    int dsize;
    uint32_t w = 0;
    (void)ud;
    if(!f) return 0;
    dsize = (int)lroundf((float)size * g_dpr);
    if(dsize <= 0) dsize = size;
    font_char_size(codepoint, FT(f), (uint32_t)dsize, &w, NULL);
    return (int)lroundf((float)w / g_dpr);
}
static void ek_font_text_size(void* ud, eweb_font_t* f, int size, const char* text, int* w, int* h) {
    int dsize;
    uint32_t ww = 0, hh = 0;
    (void)ud;
    dsize = (int)lroundf((float)size * g_dpr);
    if(dsize <= 0) dsize = size;
    if(f && text) font_text_size(text, FT(f), (uint32_t)dsize, &ww, &hh);
    if(w) *w = (int)lroundf((float)ww / g_dpr);
    if(h) *h = (int)lroundf((float)hh / g_dpr);
}
static void ek_font_draw_text(void* ud, eweb_surface_t* s, int x, int y, const char* text,
                              eweb_font_t* f, int size, uint32_t color) {
    int dsize;
    (void)ud;
    if(!s || !f || !text) return;
    /* Draw at DEVICE coordinates and rasterise the glyphs at device size for a
     * crisp result; the pen advance then matches ek_font_text_size() above. */
    dsize = (int)lroundf((float)size * S(s)->dpr);
    if(dsize <= 0) dsize = size;
    graph_draw_text_font(G(s), X(s,x), X(s,y), text, FT(f), (uint32_t)dsize, color);
}

/* ------------------------------------------------------------------ */
/* Image decode                                                        */
/* ------------------------------------------------------------------ */

/* SVG is XML text with no binary magic, so GRAPH_IMAGE_TYPE_AUTO never sniffs
 * it; detect the <svg root ourselves and rasterise explicitly. */
static bool ek_looks_like_svg(const uint8_t* data, int sz) {
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

/* WebP lives in libwebp (browser/ewebview/libwebp), which graph_image cannot
 * reach, so decode it here and copy into a graph_t. */
static graph_t* ek_webp_new_from_data(const uint8_t* data, uint32_t size) {
    webp_image_t img;
    graph_t* g;
    if(!webp_is_webp(data, size)) return NULL;
    if(webp_decode(data, size, &img) != WEBP_OK) return NULL;
    g = graph_new(NULL, img.width, img.height);
    if(g) memcpy(g->buffer, img.pixels, (size_t)img.width * img.height * sizeof(uint32_t));
    webp_image_free(&img);
    return g;
}

static eweb_surface_t* ek_image_decode(void* ud, const uint8_t* data, int size) {
    graph_t* img;
    (void)ud;
    if(!data || size <= 0) return NULL;
    if(ek_looks_like_svg(data, size)) {
        graph_t* svg = graph_image_new_from_data(GRAPH_IMAGE_TYPE_SVG, data, (uint32_t)size);
        if(svg) return ek_wrap(svg, 1.0f);
    }
    img = ek_webp_new_from_data(data, (uint32_t)size);
    if(!img) img = graph_image_new_from_data(GRAPH_IMAGE_TYPE_AUTO, data, size);
    /* Natural-pixel image: dpr == 1, so its logical size is its pixel size and
     * ek_blit scales it up to the destination frame's dpr. */
    return ek_wrap(img, 1.0f);
}

/* ------------------------------------------------------------------ */
/* Network                                                             */
/* ------------------------------------------------------------------ */

/* Per-request state kept alive between net.request() and net.free_response(). */
typedef struct {
    TinyHttpsResponse* response;
    eweb_http_header_t* headers;
    uint8_t* decoded;   /* inflate()d body copy; owns the buffer resp->body aliases */
} ek_http_t;

/* Inflate a gzip/zlib-wrapped HTTP body. CDNs (g.alicdn.com in particular)
 * gzip script and JSON payloads even when the client sent no
 * Accept-Encoding, and libtinyhttpsc hands the compressed bytes through
 * verbatim, so the decode has to happen here in the port. windowBits 15+32
 * makes zlib auto-detect the gzip vs zlib wrapper. Returns a malloc()d
 * buffer (caller frees) or NULL when the body is not compressed or the
 * stream is truncated/corrupt - the caller then keeps the raw bytes. */
static uint8_t* ek_inflate_body(const uint8_t* src, int src_len, int* out_len) {
    z_stream zs;
    uint8_t* out;
    size_t cap;
    int ret;
    if(!src || src_len <= 0 || !out_len) return NULL;
    memset(&zs, 0, sizeof(zs));
    if(inflateInit2(&zs, 15 + 32) != Z_OK) return NULL;
    cap = (size_t)src_len * 4 + 4096;
    out = (uint8_t*)malloc(cap);
    if(!out) { inflateEnd(&zs); return NULL; }
    zs.next_in  = (Bytef*)src;
    zs.avail_in = (uInt)src_len;
    for(;;) {
        zs.next_out  = out + zs.total_out;
        zs.avail_out = (uInt)(cap - zs.total_out);
        ret = inflate(&zs, Z_NO_FLUSH);
        if(ret == Z_STREAM_END) break;
        if(ret != Z_OK) { free(out); inflateEnd(&zs); return NULL; }
        if(zs.avail_out == 0) {
            size_t ncap = cap * 2;
            uint8_t* n = (uint8_t*)realloc(out, ncap);
            if(!n) { free(out); inflateEnd(&zs); return NULL; }
            out = n; cap = ncap;
            continue;
        }
        if(zs.avail_in == 0) { free(out); inflateEnd(&zs); return NULL; }
    }
    *out_len = (int)zs.total_out;
    inflateEnd(&zs);
    return out;
}

/* Percent-encode characters that are illegal raw in a URL but that pages do
 * emit unescaped in query strings (apple.com's webfont URL carries a literal
 * '|': .../wss/fonts?families=SF+Pro,v3|SF+Pro+Icons,v3). libtinyhttpsc sends
 * the request line verbatim, so the raw byte reached the server and the font
 * 404'd, leaving the nav bag glyph as a missing-glyph blob. Already-escaped
 * '%' sequences and the reserved set are left untouched to avoid double
 * encoding. Falls back to the input when it would not fit. */
static const char* ek_url_sanitize(const char* url, char* buf, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)url;
    for(; *p; p++) {
        unsigned char c = *p;
        bool unsafe = (c == ' ' || c == '"' || c == '<' || c == '>' || c == '|' ||
                       c == '{' || c == '}' || c == '\\' || c == '^' || c == '`' ||
                       c >= 0x80);
        if(unsafe) {
            if(o + 3 >= cap) return url;
            buf[o++] = '%';
            buf[o++] = hex[c >> 4];
            buf[o++] = hex[c & 0xF];
        } else {
            if(o + 1 >= cap) return url;
            buf[o++] = (char)c;
        }
    }
    buf[o] = '\0';
    return buf;
}

static bool ek_net_request(void* ud, const char* url, const char* method,
                           const char* req_body, int req_body_size,
                           const eweb_http_header_t* req_headers, int req_header_count,
                           eweb_http_response_t* resp) {
    TinyHttpsRequest* request;
    TinyHttpsResponse* response;
    ek_http_t* st;
    int i, n, body_size;
    const char* body;
    (void)ud;
    if(!url || !resp) return false;
    memset(resp, 0, sizeof(*resp));

    {
        char safe[4096];
        request = NewHttpsRequest(ek_url_sanitize(url, safe, sizeof(safe)));
    }
    if(!request) return false;

    HttpsRequestSetTimeout(request, 10000);
    HttpsRequestSetMaxRedirections(request, 0);   /* the core follows redirects */
    if(method && method[0] && strcmp(method, "GET") != 0)
        HttpsRequestSetMethod(request, method);   /* copies internally */
    HttpsRequestAddHeader(request, "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36 MyEwokoBrowser/1.0");
    HttpsRequestAddHeader(request, "Sec-CH-UA", "\"Chromium\";v=\"152\", \"Not=A?Brand\";v=\"99\", \"MyEwokoBrowser\";v=\"1.0\"");
    HttpsRequestAddHeader(request, "Sec-CH-UA-Mobile", "?0");
    HttpsRequestAddHeader(request, "Sec-CH-UA-Platform", "\"Linux\"");
    for(i = 0; i < req_header_count; i++) {
        if(req_headers[i].key)
            HttpsRequestAddHeader(request, req_headers[i].key,
                                  req_headers[i].value ? req_headers[i].value : "");
    }
    /* SendBodyStr copies the payload (default COPY strategy) and the client
     * emits Content-Length itself, so a borrowed pointer is safe here. */
    if(req_body && req_body_size > 0)
        HttpsRequestSendBodyStr(request, (char*)req_body);

    response = HttpsRequestFetch(request);
    HttpsRequestFree(request);
    if(!response) return false;

    st = (ek_http_t*)calloc(1, sizeof(ek_http_t));
    if(!st) { HttpsResponseFree(response); return false; }
    st->response = response;

    /* Copy the header key/value index out into an eweb_http_header_t array. The
     * strings themselves are owned by the response and stay valid until
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
    /* Compressed payload? Inflate into a port-owned buffer and hand THAT to
     * the core; the Content-Encoding header is rewritten to identity so the
     * (key,value) index stays truthful about the bytes we now expose. */
    if(resp->body_size > 2 && st->headers) {
        int compressed = 0;
        for(i = 0; i < n; i++) {
            const char* k = st->headers[i].key;
            const char* v = st->headers[i].value;
            if(!k || !v) continue;
            if(strcasecmp(k, "Content-Encoding") != 0) continue;
            if(strstr(v, "gzip") || strstr(v, "deflate")) compressed = 1;
        }
        if(compressed) {
            int dlen = 0;
            uint8_t* dec = ek_inflate_body(resp->body, resp->body_size, &dlen);
            if(dec) {
                st->decoded = dec;
                resp->body = dec;
                resp->body_size = dlen;
                for(i = 0; i < n; i++)
                    if(st->headers[i].key &&
                       strcasecmp(st->headers[i].key, "Content-Encoding") == 0)
                        st->headers[i].value = "identity";
            } else {
                klog("[ewebview] net: Content-Encoding gzip but "
                     "inflate failed: url=%s bytes=%d\n", url, resp->body_size);
            }
        }
    }
    /* The HAL response only carries `error`/`status`, so the core's loadURL
     * failure branches cannot show WHY a transport failed. Surface tinyhttpsc's
     * own error string/code through klog (the EwokOS log stream the rest of
     * this port uses) on a transport error or a non-2xx status, so a failed
     * fetch is diagnosable without a rebuild. */
    if(resp->error || resp->status < 200 || resp->status > 299) {
        klog("[ewebview] net.request diag: url=%s status=%d error=%d code=%d "
             "msg=%s body=%d\n",
             url, resp->status, resp->error ? 1 : 0,
             HttpsResponseGetErrorCode(response),
             HttpsResponseGetErrorMsg(response) ? HttpsResponseGetErrorMsg(response) : "(null)",
             resp->body_size);
    }
    return true;
}

static void ek_net_free_response(void* ud, eweb_http_response_t* resp) {
    ek_http_t* st;
    (void)ud;
    if(!resp) return;
    st = (ek_http_t*)resp->native;
    if(st) {
        if(st->decoded) free(st->decoded);
        if(st->headers) free(st->headers);
        if(st->response) HttpsResponseFree(st->response);
        free(st);
    }
    memset(resp, 0, sizeof(*resp));
}

static uint8_t* ek_net_read_file(void* ud, const char* path, int* out_size) {
    (void)ud;
    if(!path || !path[0]) { if(out_size) *out_size = 0; return NULL; }
    return vfs_readfile(path, out_size);   /* malloc'd; the core free()s it */
}

static const char* ek_net_resolve_resource(void* ud, const char* res, char* buf, int bufsize) {
    (void)ud;
    if(!res || !buf || bufsize <= 0) return NULL;
    /* libx resolves "name" to <app-dir>/res/name (or an absolute path as-is),
     * always writing into the caller's buffer; empty result means unknown. */
    x_get_res_name(res, buf, (uint32_t)bufsize);
    if(buf[0] == 0) return NULL;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Clock                                                               */
/* ------------------------------------------------------------------ */

/* sys_tic_ms() is the host helper in litehtml/os_types.h (clock_gettime based)
 * and lives in a C++ header, so this C99 port cannot see it - use the EwokOS
 * kernel clock from <ewoksys/kernel_tic.h> instead. */
static uint64_t ek_clock_tic_ms(void* ud) { (void)ud; return kernel_tic_ms(0); }
static void ek_clock_sleep_ms(void* ud, uint32_t ms) { (void)ud; proc_usleep(ms * 1000); }

/* ------------------------------------------------------------------ */
/* Platform utilities                                                  */
/* ------------------------------------------------------------------ */

/* From libgloss compat.c: non-dereferencing heap-membership test. */
int ewok_ptr_in_heap(const void* p);
static bool ek_sys_ptr_sane(void* ud, const void* p) { (void)ud; return ewok_ptr_in_heap(p) != 0; }

static void ek_sys_log(void* ud, const char* text) {
    (void)ud;
    if(text) klog("%s", text);
}

/* System clipboard: EwokOS keeps one global text clipboard at /tmp/.clipboard
 * (libclipboard). clipboard_get_text() already returns a malloc'd
 * NUL-terminated copy the core free()s; the HAL wants NULL when empty, so fold
 * the "" case away. clipboard_set_text() overwrites the whole clipboard. */
static char* ek_sys_clipboard_get(void* ud) {
    char* text;
    (void)ud;
    text = clipboard_get_text();
    if(text && text[0] == 0) { free(text); return NULL; }
    return text;
}
static void ek_sys_clipboard_set(void* ud, const char* text) {
    (void)ud;
    if(text) clipboard_set_text(text);
}

/* ------------------------------------------------------------------ */
/* Bundle                                                              */
/* ------------------------------------------------------------------ */

/* eweb_port_init() (a plain memset) lives in the ewebview core, not here:
 * it is platform-independent and must exist even when no port is linked. */

void eweb_port_ewokos(eweb_port_t* port, void* ud) {
    if(!port) return;
    memset(port, 0, sizeof(*port));

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

    port->sys.ud           = ud;
    port->sys.ptr_sane     = ek_sys_ptr_sane;
    port->sys.log          = ek_sys_log;
    port->sys.clipboard_get = ek_sys_clipboard_get;
    port->sys.clipboard_set = ek_sys_clipboard_set;
}

/* HiDPI mechanism: set the device-pixel ratio the port rasterises at. Call
 * BEFORE the first ewebview_set_viewport() so the frame pool allocates
 * device-sized (logical*dpr) buffers. EwokOS embedder passes 1.0 by default,
 * i.e. a plain 1x port with no scaling. */
void eweb_port_ewokos_set_dpr(float dpr) {
    g_dpr = (dpr > 0.0f) ? dpr : 1.0f;
}

/* Recover the device-pixel graph_t* backing a surface handle so an embedder
 * can blit an adopted frame straight into its window. NULL-safe. */
void* eweb_port_ewokos_surface_native(eweb_surface_t* s) {
    return s ? (void*)G(s) : NULL;
}
