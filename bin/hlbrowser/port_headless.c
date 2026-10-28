/*
 * port_headless.c - a minimal SDL-free ewebview port for ASan fault-injection.
 *
 * Why this exists: the teardown crash family (vm_close -> gc -> var_free ->
 * var_clean -> hash_map_clean dereferencing a children.buckets overwritten with
 * ARM64 prologue bytes) is a heap corruption whose WRITE happens during github
 * hydration. Catching the write needs AddressSanitizer, but ASan-on-sdlbrowser
 * deadlocks in dyld because ASan interposes while libSDL2 (sdl2-compat/SDL3) is
 * still resolving symbols. This port drops SDL entirely: it reuses the SDL2
 * port's network (libtinyhttpsc/BearSSL, which has no SDL dependency) and clock
 * (clock_gettime), and replaces the graphics/font/image tables with in-memory
 * ARGB8888 buffers + stubs. The result is a harness that loads a real page,
 * runs the full JS/DOM pipeline under ASan, and tears down - so ASan reports
 * the corruption at the write, not at the teardown detection point.
 *
 * Rendering fidelity is NOT a goal here: the corruption lives in the mario VM's
 * var graph (driven by the page's JS bundles), which is independent of exact
 * pixels. Only the REQUIRED callbacks are fully implemented; the rest are
 * minimal so the engine does not degrade a path hydration depends on.
 *
 * Plain C99. No SDL.
 */

#include <ewebview_port.h>

/* Network: libtinyhttpsc (BearSSL) - identical to the SDL2 port; no SDL dep. */
#include <tinyhttpsc/tinyhttpsc.h>
#include <tinyhttpsc/BearHttpsClientOne.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>   /* gzip/deflate Content-Encoding decode */

/* ------------------------------------------------------------------ */
/* Graphics: in-memory ARGB8888 surface                                */
/* ------------------------------------------------------------------ */

typedef struct hl_surface {
    int        w, h;
    uint32_t*  px;      /* w*h ARGB8888 */
    int        clip_x, clip_y, clip_w, clip_h;
    bool       clip_on;
} hl_surface_t;

static void hl_dims(hl_surface_t* s, int* w, int* h) {
    if(w) *w = s ? s->w : 0;
    if(h) *h = s ? s->h : 0;
}

static eweb_surface_t* hl_surface_new(void* ud, int w, int h) {
    (void)ud;
    if(w <= 0) w = 1;
    if(h <= 0) h = 1;
    hl_surface_t* s = (hl_surface_t*)calloc(1, sizeof(hl_surface_t));
    if(!s) return NULL;
    s->w = w; s->h = h;
    s->px = (uint32_t*)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if(!s->px) { free(s); return NULL; }
    s->clip_x = 0; s->clip_y = 0; s->clip_w = w; s->clip_h = h; s->clip_on = false;
    return (eweb_surface_t*)s;
}

static void hl_surface_free(void* ud, eweb_surface_t* es) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s) return;
    if(s->px) free(s->px);
    free(s);
}

static void hl_surface_dims(void* ud, eweb_surface_t* es, int* w, int* h) {
    (void)ud;
    hl_dims((hl_surface_t*)es, w, h);
}

static uint32_t* hl_surface_pixels(void* ud, eweb_surface_t* es, int* w, int* h) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s) return NULL;
    hl_dims(s, w, h);
    return s->px;
}

static void* hl_surface_native(void* ud, eweb_surface_t* es) {
    (void)ud; (void)es;
    return NULL;
}

static void hl_surface_clear(void* ud, eweb_surface_t* es, uint32_t color) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s || !s->px) return;
    int n = s->w * s->h;
    for(int i = 0; i < n; i++) s->px[i] = color;
}

static void hl_surface_set_clip(void* ud, eweb_surface_t* es, int x, int y, int w, int h) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s) return;
    s->clip_x = x; s->clip_y = y; s->clip_w = w; s->clip_h = h; s->clip_on = true;
}

static void hl_surface_unset_clip(void* ud, eweb_surface_t* es) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s) return;
    s->clip_x = 0; s->clip_y = 0; s->clip_w = s->w; s->clip_h = s->h; s->clip_on = false;
}

static void hl_set_px(hl_surface_t* s, int x, int y, uint32_t color) {
    if(!s || !s->px) return;
    if(x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    if(s->clip_on && (x < s->clip_x || y < s->clip_y ||
                      x >= s->clip_x + s->clip_w || y >= s->clip_y + s->clip_h))
        return;
    s->px[y * s->w + x] = color;
}

static void hl_fill_rect(void* ud, eweb_surface_t* es, int x, int y, int w, int h, uint32_t color) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s) return;
    for(int j = y; j < y + h; j++)
        for(int i = x; i < x + w; i++)
            hl_set_px(s, i, j, color);
}

static void hl_rect(void* ud, eweb_surface_t* es, int x, int y, int w, int h, uint32_t color) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s) return;
    for(int i = x; i < x + w; i++) { hl_set_px(s, i, y, color); hl_set_px(s, i, y + h - 1, color); }
    for(int j = y; j < y + h; j++) { hl_set_px(s, x, j, color); hl_set_px(s, x + w - 1, j, color); }
}

static void hl_line(void* ud, eweb_surface_t* es, int x0, int y0, int x1, int y1, uint32_t color) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for(;;) {
        hl_set_px(s, x0, y0, color);
        if(x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if(e2 >= dy) { err += dy; x0 += sx; }
        if(e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void hl_wline(void* ud, eweb_surface_t* es, int x0, int y0, int x1, int y1, int w, uint32_t color) {
    (void)w;
    hl_line(ud, es, x0, y0, x1, y1, color);
}

/* Filled shapes: a bounding-box fill is enough for the VM/DOM repro; the exact
 * glyph/shape pixels do not influence the mario var graph. */
static void hl_fill_circle(void* ud, eweb_surface_t* es, int x, int y, int radius, uint32_t color) {
    hl_fill_rect(ud, es, x - radius, y - radius, radius * 2, radius * 2, color);
}
static void hl_circle(void* ud, eweb_surface_t* es, int x, int y, int radius, int rw, uint32_t color) {
    (void)rw;
    hl_rect(ud, es, x - radius, y - radius, radius * 2, radius * 2, color);
}
static void hl_arc(void* ud, eweb_surface_t* es, int x, int y, int radius, int rw,
                   float a0, float a1, uint32_t color) {
    (void)a0; (void)a1;
    hl_circle(ud, es, x, y, radius, rw, color);
}
static void hl_fill_arc(void* ud, eweb_surface_t* es, int x, int y, int radius,
                        float a0, float a1, uint32_t color) {
    (void)a0; (void)a1;
    hl_fill_circle(ud, es, x, y, radius, color);
}
static void hl_round(void* ud, eweb_surface_t* es, int x, int y, int w, int h, int radius, int rw, uint32_t color) {
    (void)radius; (void)rw;
    hl_rect(ud, es, x, y, w, h, color);
}
static void hl_fill_round(void* ud, eweb_surface_t* es, int x, int y, int w, int h, int radius, uint32_t color) {
    (void)radius;
    hl_fill_rect(ud, es, x, y, w, h, color);
}
static void hl_stroke_quadratic(void* ud, eweb_surface_t* es, int x0, int y0, int cx, int cy,
                                int x1, int y1, int w, uint32_t color) {
    (void)cx; (void)cy; (void)w;
    hl_line(ud, es, x0, y0, x1, y1, color);
}
static void hl_stroke_bezier(void* ud, eweb_surface_t* es, int x0, int y0, int cx1, int cy1,
                             int cx2, int cy2, int x1, int y1, int w, uint32_t color) {
    (void)cx1; (void)cy1; (void)cx2; (void)cy2; (void)w;
    hl_line(ud, es, x0, y0, x1, y1, color);
}

static void hl_set_pixel(void* ud, eweb_surface_t* es, int x, int y, uint32_t color) {
    (void)ud;
    hl_set_px((hl_surface_t*)es, x, y, color);
}

static uint32_t hl_get_pixel(void* ud, eweb_surface_t* es, int x, int y) {
    (void)ud;
    hl_surface_t* s = (hl_surface_t*)es;
    if(!s || !s->px || x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    return s->px[y * s->w + x];
}

static void hl_blit(void* ud, eweb_surface_t* esrc, int sx, int sy, int sw, int sh,
                    eweb_surface_t* edst, int dx, int dy, int dw, int dh) {
    (void)ud; (void)dw; (void)dh;
    hl_surface_t* src = (hl_surface_t*)esrc;
    hl_surface_t* dst = (hl_surface_t*)edst;
    if(!src || !dst || !src->px || !dst->px) return;
    for(int j = 0; j < sh; j++)
        for(int i = 0; i < sw; i++)
            hl_set_px(dst, dx + i, dy + j, hl_get_pixel(ud, esrc, sx + i, sy + j));
}

static void hl_blit_fit_alpha(void* ud, eweb_surface_t* esrc, int sx, int sy, int sw, int sh,
                              eweb_surface_t* edst, int dx, int dy, int dw, int dh, uint8_t alpha) {
    (void)alpha;
    hl_surface_t* src = (hl_surface_t*)esrc;
    hl_surface_t* dst = (hl_surface_t*)edst;
    if(!src || !dst || !src->px || !dst->px || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for(int j = 0; j < dh; j++) {
        int syy = sy + (j * sh) / dh;
        for(int i = 0; i < dw; i++) {
            int sxx = sx + (i * sw) / dw;
            hl_set_px(dst, dx + i, dy + j, hl_get_pixel(ud, esrc, sxx, syy));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Font: fixed-metrics stub (glyphs are irrelevant to the VM repro).   */
/* ------------------------------------------------------------------ */

typedef struct hl_font { int dummy; } hl_font_t;

static eweb_font_t* hl_font_create(void* ud, const char* family) {
    (void)ud; (void)family;
    return (eweb_font_t*)calloc(1, sizeof(hl_font_t));
}

static eweb_font_t* hl_font_create_styled(void* ud, const char* family, int weight, int italic) {
    (void)weight; (void)italic;
    return hl_font_create(ud, family);
}

static void hl_font_destroy(void* ud, eweb_font_t* f) {
    (void)ud;
    if(f) free(f);
}

static void hl_font_metrics(void* ud, eweb_font_t* f, int size, eweb_font_metrics_t* out) {
    (void)ud; (void)f;
    if(size <= 0) size = 16;
    if(out) {
        out->ascent  = (size * 4) / 5;
        out->descent = size - out->ascent;
        out->height  = size;
        out->x_height = size / 2;
    }
}

static int hl_font_char_width(void* ud, eweb_font_t* f, int size, uint32_t codepoint) {
    (void)ud; (void)f;
    if(size <= 0) size = 16;
    /* CJK / wide glyphs are ~full-width; ASCII ~half. Good enough for layout. */
    return (codepoint >= 0x2E80) ? size : (size + 1) / 2;
}

static void hl_font_text_size(void* ud, eweb_font_t* f, int size, const char* text, int* w, int* h) {
    (void)ud; (void)f;
    if(size <= 0) size = 16;
    int width = 0;
    if(text) {
        for(const unsigned char* p = (const unsigned char*)text; *p; ) {
            uint32_t cp;
            int adv;
            if(*p < 0x80) { cp = *p; p += 1; }
            else if((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; p += 2; }
            else if((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; p += 3; }
            else { cp = *p & 0x07; p += 4; }
            adv = (cp >= 0x2E80) ? size : (size + 1) / 2;
            width += adv;
        }
    }
    if(w) *w = width;
    if(h) *h = size;
}

static void hl_font_draw_text(void* ud, eweb_surface_t* es, int x, int y, const char* text,
                              eweb_font_t* f, int size, uint32_t color) {
    (void)ud; (void)es; (void)x; (void)y; (void)text; (void)f; (void)size; (void)color;
    /* no-op: glyph pixels do not affect the VM/DOM var graph */
}

/* ------------------------------------------------------------------ */
/* Image: degrade (NULL) - fetch still happens, pixels irrelevant.     */
/* ------------------------------------------------------------------ */

static eweb_surface_t* hl_image_decode(void* ud, const uint8_t* data, int size) {
    (void)ud; (void)data; (void)size;
    return NULL;   /* unsupported -> core degrades image rendering, JS unaffected */
}

/* ------------------------------------------------------------------ */
/* Network (verbatim from the SDL2 port; no SDL dependency).           */
/* ------------------------------------------------------------------ */

typedef struct {
    TinyHttpsResponse*   response;
    eweb_http_header_t*  headers;
    uint8_t*             decoded;
} hl_http_t;

static uint8_t* hl_inflate_body(const uint8_t* src, int src_len, int* out_len) {
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

static const char* hl_url_sanitize(const char* url, char* buf, size_t cap) {
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

static bool hl_net_request(void* ud, const char* url, const char* method,
                           const char* req_body, int req_body_size,
                           const eweb_http_header_t* req_headers, int req_header_count,
                           eweb_http_response_t* resp) {
    TinyHttpsRequest* request;
    TinyHttpsResponse* response;
    hl_http_t* st;
    int i, n, body_size;
    const char* body;
    (void)ud;
    if(!url || !resp) return false;
    memset(resp, 0, sizeof(*resp));

    {
        char safe[4096];
        request = NewHttpsRequest(hl_url_sanitize(url, safe, sizeof(safe)));
    }
    if(!request) return false;

    HttpsRequestSetTimeout(request, 10000);
    HttpsRequestSetMaxRedirections(request, 0);
    if(method && method[0] && strcmp(method, "GET") != 0)
        HttpsRequestSetMethod(request, method);
    HttpsRequestAddHeader(request, "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36 MyEwokoBrowser/1.0");
    HttpsRequestAddHeader(request, "Sec-CH-UA", "\"Chromium\";v=\"152\", \"Not=A?Brand\";v=\"99\", \"MyEwokoBrowser\";v=\"1.0\"");
    HttpsRequestAddHeader(request, "Sec-CH-UA-Mobile", "?0");
    HttpsRequestAddHeader(request, "Sec-CH-UA-Platform", "\"Linux\"");
    for(i = 0; i < req_header_count; i++) {
        if(req_headers[i].key)
            HttpsRequestAddHeader(request, req_headers[i].key,
                                  req_headers[i].value ? req_headers[i].value : "");
    }
    if(req_body && req_body_size > 0)
        HttpsRequestSendBodyStr(request, (char*)req_body);

    response = HttpsRequestFetch(request);
    HttpsRequestFree(request);
    if(!response) return false;

    st = (hl_http_t*)calloc(1, sizeof(hl_http_t));
    if(!st) { HttpsResponseFree(response); return false; }
    st->response = response;

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
    resp->body      = (uint8_t*)body;
    resp->body_size = (body && body_size > 0) ? body_size : 0;
    resp->native    = st;
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
            uint8_t* dec = hl_inflate_body(resp->body, resp->body_size, &dlen);
            if(dec) {
                st->decoded = dec;
                resp->body = dec;
                resp->body_size = dlen;
                for(i = 0; i < n; i++)
                    if(st->headers[i].key &&
                       strcasecmp(st->headers[i].key, "Content-Encoding") == 0)
                        st->headers[i].value = "identity";
            }
        }
    }
    return true;
}

static void hl_net_free_response(void* ud, eweb_http_response_t* resp) {
    hl_http_t* st;
    (void)ud;
    if(!resp) return;
    st = (hl_http_t*)resp->native;
    if(st) {
        if(st->decoded) free(st->decoded);
        if(st->headers) free(st->headers);
        if(st->response) HttpsResponseFree(st->response);
        free(st);
    }
    memset(resp, 0, sizeof(*resp));
}

static uint8_t* hl_net_read_file(void* ud, const char* path, int* out_size) {
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
    buf = (uint8_t*)malloc((size_t)sz + 1);
    if(!buf) { fclose(fp); return NULL; }
    got = (sz > 0) ? fread(buf, 1, (size_t)sz, fp) : 0;
    fclose(fp);
    buf[got] = 0;
    if(out_size) *out_size = (int)got;
    return buf;
}

static const char* hl_net_resolve_resource(void* ud, const char* res, char* buf, int bufsize) {
    int n;
    (void)ud;
    if(!res || !buf || bufsize <= 0) return NULL;
    if(res[0] == '/')
        n = snprintf(buf, (size_t)bufsize, "%s", res);
    else
        return NULL;   /* no res:// base in the headless harness */
    if(n <= 0 || n >= bufsize || buf[0] == 0) return NULL;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Clock: CLOCK_MONOTONIC (matches litehtml's internal sys_tic_ms).    */
/* ------------------------------------------------------------------ */

static uint64_t hl_clock_tic_ms(void* ud) {
    (void)ud;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void hl_clock_sleep_ms(void* ud, uint32_t ms) {
    (void)ud;
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Platform utilities                                                  */
/* ------------------------------------------------------------------ */

static void hl_sys_log(void* ud, const char* text) {
    (void)ud;
    if(text) fprintf(stderr, "%s\n", text);
}

/* ------------------------------------------------------------------ */
/* Bundle                                                              */
/* ------------------------------------------------------------------ */

void eweb_port_headless(eweb_port_t* port, void* ud) {
    if(!port) return;
    memset(port, 0, sizeof(*port));

    port->gfx.ud = ud;
    port->gfx.surface_new        = hl_surface_new;
    port->gfx.surface_free       = hl_surface_free;
    port->gfx.surface_dims       = hl_surface_dims;
    port->gfx.surface_pixels     = hl_surface_pixels;
    port->gfx.surface_native     = hl_surface_native;
    port->gfx.surface_clear      = hl_surface_clear;
    port->gfx.surface_set_clip   = hl_surface_set_clip;
    port->gfx.surface_unset_clip = hl_surface_unset_clip;
    port->gfx.fill_rect          = hl_fill_rect;
    port->gfx.rect               = hl_rect;
    port->gfx.line               = hl_line;
    port->gfx.wline              = hl_wline;
    port->gfx.circle             = hl_circle;
    port->gfx.fill_circle        = hl_fill_circle;
    port->gfx.arc                = hl_arc;
    port->gfx.fill_arc           = hl_fill_arc;
    port->gfx.round              = hl_round;
    port->gfx.fill_round         = hl_fill_round;
    port->gfx.stroke_quadratic   = hl_stroke_quadratic;
    port->gfx.stroke_bezier      = hl_stroke_bezier;
    port->gfx.flatten_quadratic  = NULL;   /* OPTIONAL: core falls back to chords */
    port->gfx.flatten_cubic      = NULL;
    port->gfx.set_pixel          = hl_set_pixel;
    port->gfx.get_pixel          = hl_get_pixel;
    port->gfx.blit               = hl_blit;
    port->gfx.blit_fit_alpha     = hl_blit_fit_alpha;

    port->font.ud            = ud;
    port->font.create        = hl_font_create;
    port->font.destroy       = hl_font_destroy;
    port->font.metrics       = hl_font_metrics;
    port->font.char_width    = hl_font_char_width;
    port->font.text_size     = hl_font_text_size;
    port->font.draw_text     = hl_font_draw_text;
    port->font.create_styled = hl_font_create_styled;

    port->image.ud     = ud;
    port->image.decode = hl_image_decode;

    port->net.ud               = ud;
    port->net.request          = hl_net_request;
    port->net.free_response    = hl_net_free_response;
    port->net.read_file        = hl_net_read_file;
    port->net.resolve_resource = hl_net_resolve_resource;

    port->clock.ud       = ud;
    port->clock.tic_ms   = hl_clock_tic_ms;
    port->clock.sleep_ms = hl_clock_sleep_ms;

    port->sys.ud       = ud;
    port->sys.ptr_sane = NULL;   /* OPTIONAL: degrade to liveness tag */
    port->sys.log      = hl_sys_log;
    port->sys.clipboard_get = NULL;
    port->sys.clipboard_set = NULL;
}
