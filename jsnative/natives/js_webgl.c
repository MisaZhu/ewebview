/*
 * js_webgl.c - a software WebGL 1.0 (OpenGL ES 2.0) backend for the mario VM.
 *
 * See js_webgl.h for the contract and the honesty red line. Design:
 *   - One heap gl_ctx per WebGLRenderingContext; its var->value is the gl_ctx,
 *     torn down via on_destroy (it holds a ref'd canvas Element var) + free_func.
 *   - GLSL shading is delegated to js_glsl.c: the vertex shader runs once per
 *     vertex (producing gl_Position + interpolated varyings), the fragment
 *     shader once per covered pixel (producing gl_FragColor). Attribute /
 *     uniform / varying plumbing crosses the interpreter boundary through the
 *     name-based getv/setv callbacks, backed by small name->value tables here.
 *   - A CPU rasterizer covers POINTS / LINES / LINE_STRIP / LINE_LOOP /
 *     TRIANGLES / TRIANGLE_STRIP / TRIANGLE_FAN with barycentric interpolation.
 *   - The framebuffer (0xAARRGGBB) is flushed into the embedder canvas through
 *     the SAME set_pixel callback the 2D context uses, so canvas.toDataURL()
 *     reads back the genuinely rendered pixels with no canvas-bridge changes.
 *   - The reported GL identity is HONEST: masked VENDOR/RENDERER use the WebGL
 *     spec defaults, unmasked strings name the real software rasterizer.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "js_webgl.h"
#include "js_glsl.h"
#include "js_canvas.h"
#include "js_natives_priv.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* WebGL / OpenGL ES 2.0 enum values (numeric, exactly what scripts pass) */
/* ------------------------------------------------------------------ */
#define GL_DEPTH_BUFFER_BIT        0x00000100
#define GL_STENCIL_BUFFER_BIT      0x00000400
#define GL_COLOR_BUFFER_BIT        0x00004000

#define GL_POINTS                  0x0000
#define GL_LINES                   0x0001
#define GL_LINE_LOOP               0x0002
#define GL_LINE_STRIP              0x0003
#define GL_TRIANGLES               0x0004
#define GL_TRIANGLE_STRIP          0x0005
#define GL_TRIANGLE_FAN            0x0006

#define GL_ZERO                    0
#define GL_ONE                     1
#define GL_SRC_COLOR               0x0300
#define GL_ONE_MINUS_SRC_COLOR     0x0301
#define GL_SRC_ALPHA               0x0302
#define GL_ONE_MINUS_SRC_ALPHA     0x0303
#define GL_DST_ALPHA               0x0304
#define GL_ONE_MINUS_DST_ALPHA     0x0305
#define GL_DST_COLOR               0x0306
#define GL_ONE_MINUS_DST_COLOR     0x0307

#define GL_CULL_FACE               0x0400
#define GL_DEPTH_TEST              0x0B71
#define GL_BLEND                   0x0BE2
#define GL_SCISSOR_TEST            0x0C11
#define GL_DITHER                  0x0BD0

#define GL_FRONT                   0x0404
#define GL_BACK                    0x0405
#define GL_CW                      0x0900
#define GL_CCW                     0x0901

#define GL_NEVER                   0x0200
#define GL_LESS                    0x0201
#define GL_LEQUAL                  0x0203
#define GL_GREATER                 0x0204
#define GL_GEQUAL                  0x0206
#define GL_EQUAL                   0x0202
#define GL_NOTEQUAL                0x0205
#define GL_ALWAYS                  0x0207

#define GL_BYTE                    0x1400
#define GL_UNSIGNED_BYTE           0x1401
#define GL_SHORT                   0x1402
#define GL_UNSIGNED_SHORT          0x1403
#define GL_INT                     0x1404
#define GL_UNSIGNED_INT            0x1405
#define GL_FLOAT                   0x1406
#define GL_FIXED                   0x140C

#define GL_ARRAY_BUFFER            0x8892
#define GL_ELEMENT_ARRAY_BUFFER    0x8893
#define GL_STREAM_DRAW             0x88E0
#define GL_STATIC_DRAW             0x88E4
#define GL_DYNAMIC_DRAW            0x88E8

#define GL_FRAGMENT_SHADER         0x8B30
#define GL_VERTEX_SHADER           0x8B31
#define GL_COMPILE_STATUS          0x8B81
#define GL_LINK_STATUS             0x8B82
#define GL_VALIDATE_STATUS         0x8B83
#define GL_SHADER_TYPE             0x8B4F
#define GL_DELETE_STATUS           0x8B80
#define GL_INFO_LOG_LENGTH         0x8B84

#define GL_VENDOR                  0x1F00
#define GL_RENDERER                0x1F01
#define GL_VERSION                 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAX_TEXTURE_SIZE        0x0D33
#define GL_MAX_RENDERBUFFER_SIZE   0x84E8
#define GL_MAX_VERTEX_ATTRIBS      0x8869
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#define GL_MAX_VARYING_VECTORS     0x8DFC
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#define GL_MAX_VIEWPORT_DIMS       0x0D3A
#define GL_ALIASED_LINE_WIDTH_RANGE 0x846E
#define GL_ALIASED_POINT_SIZE_RANGE 0x846D
#define GL_RED_BITS                0x0D52
#define GL_GREEN_BITS              0x0D53
#define GL_BLUE_BITS               0x0D54
#define GL_ALPHA_BITS              0x0D55
#define GL_DEPTH_BITS              0x0D56
#define GL_STENCIL_BITS            0x0D57
#define GL_UNPACK_FLIP_Y_WEBGL     0x9240
#define GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL 0x9241
#define GL_UNPACK_COLORSPACE_CONVERSION_WEBGL 0x9243
#define GL_CONTEXT_LOST_WEBGL      0x9242
#define GL_UNMASKED_VENDOR_WEBGL   0x9245
#define GL_UNMASKED_RENDERER_WEBGL 0x9246

#define GL_NO_ERROR                0
#define GL_TEXTURE_2D              0x0DE1
#define GL_TEXTURE0                0x84C0

/* Texture object state enums (WebGL 1.0 / OpenGL ES 2.0 values). */
#define GL_NEAREST                 0x2600
#define GL_LINEAR                  0x2601
#define GL_NEAREST_MIPMAP_NEAREST  0x2700
#define GL_LINEAR_MIPMAP_NEAREST   0x2701
#define GL_NEAREST_MIPMAP_LINEAR   0x2702
#define GL_LINEAR_MIPMAP_LINEAR    0x2703
#define GL_TEXTURE_MAG_FILTER      0x2800
#define GL_TEXTURE_MIN_FILTER      0x2801
#define GL_TEXTURE_WRAP_S          0x2802
#define GL_TEXTURE_WRAP_T          0x2803
#define GL_REPEAT                  0x2901
#define GL_CLAMP_TO_EDGE           0x812F
#define GL_MIRRORED_REPEAT         0x8370
#define GL_RGBA                    0x1908
#define GL_RGB                     0x1907
#define GL_LUMINANCE               0x1909
#define GL_ALPHA                   0x1906
#define GL_LUMINANCE_ALPHA         0x190A
#define GL_UNPACK_ALIGNMENT        0x0CF5
#define GL_TEXTURE_BINDING_2D      0x8069
#define GL_MAX_TEXTURE_UNITS       8

/* Framebuffer / renderbuffer object enums (WebGL 1.0). */
#define GL_FRAMEBUFFER             0x8D40
#define GL_RENDERBUFFER            0x8D41
#define GL_COLOR_ATTACHMENT0       0x8CE0
#define GL_DEPTH_ATTACHMENT        0x8D00
#define GL_STENCIL_ATTACHMENT      0x8D20
#define GL_DEPTH_COMPONENT16       0x81A5
#define GL_FRAMEBUFFER_COMPLETE    0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_UNSUPPORTED 0x8CDD
#define GL_FRAMEBUFFER_BINDING     0x8CA6
#define GL_RENDERBUFFER_BINDING    0x8CA7

/* getShaderPrecisionFormat precision qualifiers. */
#define GL_LOW_FLOAT               0x8DF0
#define GL_MEDIUM_FLOAT            0x8DF1
#define GL_HIGH_FLOAT              0x8DF2
#define GL_LOW_INT                 0x8DF3
#define GL_MEDIUM_INT              0x8DF4
#define GL_HIGH_INT                0x8DF5

#define GL_MAX_ATTRIBS   16
#define GL_MAX_NAMEVAL   32
#define GL_MAX_VERTS     65536

/* ------------------------------------------------------------------ */
/* GL object model (pure C)                                            */
/* ------------------------------------------------------------------ */
typedef struct gl_buffer {
    float* fdata;      /* element values stored as float (Float32Array path) */
    int    nf;
    int    usage;
    int    deleted;
} gl_buffer;

typedef struct gl_shader {
    int          type;         /* GL_VERTEX_SHADER / GL_FRAGMENT_SHADER */
    char*        source;
    glsl_shader* prog;         /* compiled (NULL if compile failed) */
    int          compile_status;
    char*        info_log;
    int          deleted;
} gl_shader;

typedef struct gl_uniform {
    char*  name;
    int    n;                  /* component count (1..4; arrays flatten; mat4=16) */
    int    mat;                /* 0 = scalar/vector; 2/3/4 = matN (c[] column-major) */
    int    samp;               /* 1 = sampler2D (c[0] = texture unit index) */
    float  c[16];
} gl_uniform;

/* A TEXTURE_2D object: RGBA8 storage regardless of the upload format, plus
 * the sampler state texture2D() sampling honours (filter + wrap). */
typedef struct gl_texture {
    uint8_t* rgba;             /* w*h*4, row 0 = texture v=0 (bottom) */
    int      w, h;
    int      min_filter, mag_filter;
    int      wrap_s, wrap_t;
    int      deleted;
} gl_texture;

/* A framebuffer object: its own color store (0xAARRGGBB, row 0 = top) sized
 * from the COLOR_ATTACHMENT0 texture. Every flush while it is bound copies
 * the store back into the attached texture (fb top row = texture v=1), so
 * sampling the texture after unbind sees the rendered pixels. */
typedef struct gl_fbo {
    uint32_t*  fb;
    float*     depth;
    int        w, h;
    gl_texture* color;         /* COLOR_ATTACHMENT0 (borrowed, pinned below) */
    var_t*     color_var;
    int        has_depth;      /* DEPTH_ATTACHMENT renderbuffer attached */
    int        deleted;
} gl_fbo;

/* A renderbuffer object: storage description only. Depth attachments back
 * the fbo's depth buffer; the rasterizer does not depth-test yet (same
 * limitation as the default framebuffer). */
typedef struct gl_rbo {
    int w, h;
    int fmt;
    int deleted;
} gl_rbo;

typedef struct gl_program {
    gl_shader*   vs;
    gl_shader*   fs;
    /* Attached shader vars are pinned: the program borrows the gl_shader
     * payloads, so JS dropping the last shader reference must not free them
     * out from under a later link/draw (released in on_destroy). */
    var_t*       vs_var;
    var_t*       fs_var;
    int          link_status;
    char*        info_log;
    /* attribute reflection: name -> location (index) */
    char*        attr_name[GL_MAX_ATTRIBS];
    int          nattr;
    /* uniform table (owned by the program) */
    gl_uniform** uniforms;
    int          nuniform, ucap;
    int          deleted;
} gl_program;

typedef struct {
    int          enabled;
    gl_buffer*   buffer;
    var_t*       buffer_var;   /* pinned: fetch_vertex reads buffer->fdata */
    int          size;         /* components per vertex (1..4) */
    int          type;         /* GL_FLOAT etc */
    int          normalized;
    int          stride;       /* bytes */
    int          offset;       /* bytes */
} gl_attrib;

typedef struct gl_ctx {
    int      w, h;
    uint32_t* fb;              /* framebuffer, 0xAARRGGBB, row 0 = top */
    float*   depth;
    /* dirty rect for the embedder flush (inclusive); x0>x1 means empty */
    int      dx0, dy0, dx1, dy1;
    uint32_t clear_color;      /* 0xAARRGGBB */
    float    clear_r, clear_g, clear_b, clear_a;
    int      vp_x, vp_y, vp_w, vp_h;
    int      blend_enabled, depth_test_enabled, cull_enabled, scissor_enabled;
    int      cull_mode, front_face;
    int      blend_src, blend_dst;
    gl_program* program;
    gl_buffer*  array_buffer;
    gl_buffer*  element_buffer;
    /* GL bindings borrow JS-owned payloads: pin the bound vars so the GC
     * cannot free a buffer/program/texture still referenced by native state
     * (heap-use-after-free in fetch_vertex when a page drops its last ref).
     * All pins are released in gl_ctx_on_destroy, never in the free_func. */
    var_t*      program_var;
    var_t*      array_buffer_var;
    var_t*      element_buffer_var;
    gl_attrib   attrib[GL_MAX_ATTRIBS];
    /* texture pipeline: per-unit TEXTURE_2D bindings + unpack pixel store */
    gl_texture* tex_unit[GL_MAX_TEXTURE_UNITS];
    var_t*      tex_unit_var[GL_MAX_TEXTURE_UNITS];
    int         active_unit;
    int         unpack_flip_y, unpack_premult, unpack_alignment;
    /* FBO bindings (NULL = default framebuffer); pinned like the rest. */
    gl_fbo*     fbo;
    var_t*      fbo_var;
    gl_rbo*     rbo;
    var_t*      rbo_var;
    /* embedder flush path */
    void*    embed_ctx;
    void*    canvas;
    webgl_set_pixel_fn setpx;
    var_t*   canvas_el;        /* ref'd; returned by gl.canvas */
    js_canvas_callbacks_t cb;  /* copied; texImage2D image-source resolution */
} gl_ctx;

/* Name->value table used to cross the GLSL interpreter boundary. */
typedef struct { const char* name; glsl_val v; } gl_nv;

/* Per-vertex shader output. */
typedef struct {
    float   clip[4];
    float   pointsize;
    gl_nv   vary[GL_MAX_NAMEVAL];
    int     nvary;
} gl_vertout;

/* Shading callback context. */
typedef struct {
    gl_ctx*     gl;
    gl_program* prog;
    gl_nv*      in;  int nin;    /* inputs (attributes or interpolated varyings) */
    gl_vertout* out;             /* vertex stage output (NULL for fragment) */
    glsl_val    fragcolor;
    int         have_frag;
} shade_ud;

/* ------------------------------------------------------------------ */
/* Pinned class prototypes (see the memory guardrails: C-side built-in  */
/* classes are pinned at register time and instances are built with     */
/* var_new_obj(proto) directly - never looked up by global name, which  */
/* a page could overwrite to hijack construction).                     */
/* ------------------------------------------------------------------ */
static var_t* g_ctx_cls   = NULL;  static var_t* g_ctx_proto   = NULL;
static var_t* g_buf_cls   = NULL;  static var_t* g_buf_proto   = NULL;
static var_t* g_shd_cls   = NULL;  static var_t* g_shd_proto   = NULL;
static var_t* g_prg_cls   = NULL;  static var_t* g_prg_proto   = NULL;
static var_t* g_uloc_cls  = NULL;  static var_t* g_uloc_proto  = NULL;
static var_t* g_ainfo_cls = NULL;  static var_t* g_ainfo_proto = NULL;
static var_t* g_tex_cls   = NULL;  static var_t* g_tex_proto   = NULL;
static var_t* g_fbo_cls   = NULL;  static var_t* g_fbo_proto   = NULL;
static var_t* g_rbo_cls   = NULL;  static var_t* g_rbo_proto   = NULL;
/* The VM the pins above belong to. Every navigation closes the page VM and
 * builds a fresh one, so the pins are per-VM state: a process-wide "registered
 * once" flag left them pointing into the closed VM's freed heap, and the next
 * page's getContext('webgl') built its context on a dangling prototype
 * (heap-use-after-free in var_add; a data abort on EwokOS once the trunk
 * allocator recycled the block). */
static vm_t*  g_reg_vm = NULL;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static void gl_mark_dirty(gl_ctx* g, int x, int y) {
    if(x < g->dx0) g->dx0 = x;
    if(y < g->dy0) g->dy0 = y;
    if(x > g->dx1) g->dx1 = x;
    if(y > g->dy1) g->dy1 = y;
}
static void gl_dirty_full(gl_ctx* g) {
    g->dx0 = 0; g->dy0 = 0; g->dx1 = g->w - 1; g->dy1 = g->h - 1;
}

/* Lazily size an FBO from its attached color texture (the texture may get
 * its storage via texImage2D after the attachment). Returns 1 when the fbo
 * has a usable color store. */
static int fbo_ensure(gl_fbo* f) {
    if(f == NULL) return 0;
    if(f->fb != NULL) return 1;
    gl_texture* t = f->color;
    if(t == NULL || t->rgba == NULL || t->w <= 0 || t->h <= 0) return 0;
    f->w = t->w; f->h = t->h;
    f->fb = (uint32_t*)calloc((size_t)f->w * (size_t)f->h, sizeof(uint32_t));
    if(f->fb == NULL) return 0;
    f->depth = (float*)calloc((size_t)f->w * (size_t)f->h, sizeof(float));
    if(f->depth != NULL)
        for(int i = 0; i < f->w * f->h; ++i) f->depth[i] = 1.0f;
    return 1;
}

/* The active render target: bound FBO or the default framebuffer. */
static uint32_t* gl_target(gl_ctx* g, int* w, int* h, float** depth) {
    if(g->fbo != NULL && fbo_ensure(g->fbo)) {
        *w = g->fbo->w; *h = g->fbo->h;
        if(depth != NULL) *depth = g->fbo->depth;
        return g->fbo->fb;
    }
    *w = g->w; *h = g->h;
    if(depth != NULL) *depth = g->depth;
    return g->fb;
}

/* Push the dirty rect of the framebuffer into the embedder canvas so
 * toDataURL()/compositing see the rendered pixels. While an FBO is bound
 * the flush instead syncs its store into the attached color texture. */
static void gl_flush(gl_ctx* g) {
    if(g->fbo != NULL) {
        gl_fbo* f = g->fbo;
        gl_texture* t = f->color;
        if(fbo_ensure(f) && t != NULL && t->rgba != NULL) {
            int cw = f->w < t->w ? f->w : t->w;
            int ch = f->h < t->h ? f->h : t->h;
            for(int ty = 0; ty < ch; ++ty) {
                /* fb row 0 = top = texture v=1; texture row 0 = v=0. */
                const uint32_t* src = f->fb + (size_t)(f->h - 1 - ty) * f->w;
                uint8_t* dst = t->rgba + (size_t)ty * t->w * 4;
                for(int x = 0; x < cw; ++x) {
                    uint32_t c = src[x];
                    dst[x*4+0] = (uint8_t)(c >> 16);
                    dst[x*4+1] = (uint8_t)(c >> 8);
                    dst[x*4+2] = (uint8_t)c;
                    dst[x*4+3] = (uint8_t)(c >> 24);
                }
            }
        }
        g->dx0 = 1; g->dy0 = 1; g->dx1 = 0; g->dy1 = 0;
        return;
    }
    if(g->setpx == NULL || g->canvas == NULL) return;
    if(g->dx0 > g->dx1 || g->dy0 > g->dy1) return;   /* empty */
    int x0 = g->dx0 < 0 ? 0 : g->dx0;
    int y0 = g->dy0 < 0 ? 0 : g->dy0;
    int x1 = g->dx1 > g->w - 1 ? g->w - 1 : g->dx1;
    int y1 = g->dy1 > g->h - 1 ? g->h - 1 : g->dy1;
    for(int y = y0; y <= y1; ++y)
        for(int x = x0; x <= x1; ++x)
            g->setpx(g->embed_ctx, g->canvas, x, y, g->fb[y * g->w + x]);
    g->dx0 = 1; g->dy0 = 1; g->dx1 = 0; g->dy1 = 0;   /* reset to empty */
}

static uint8_t clamp255(float v) {
    if(!(v > 0.0f)) return 0;             /* NaN / <=0 */
    if(v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}
static uint32_t pack_rgba(float r, float g, float b, float a) {
    return ((uint32_t)clamp255(a) << 24) | ((uint32_t)clamp255(r) << 16) |
           ((uint32_t)clamp255(g) << 8)  | (uint32_t)clamp255(b);
}

/* Source-over blend of src onto dst (both 0xAARRGGBB). */
static uint32_t blend_srcover(uint32_t dst, uint32_t src) {
    int sa = (src >> 24) & 0xff;
    if(sa == 255) return src;
    if(sa == 0)   return dst;
    int da = (dst >> 24) & 0xff;
    int inv = 255 - sa;
    int oa = sa + (da * inv) / 255;
    if(oa == 0) return 0;
    int ch[3];
    for(int i = 0; i < 3; ++i) {
        int sh = (src >> (16 - i * 8)) & 0xff;
        int dh = (dst >> (16 - i * 8)) & 0xff;
        ch[i] = (sh * sa + dh * da * inv / 255) / oa;
    }
    return ((uint32_t)oa << 24) | ((uint32_t)ch[0] << 16) |
           ((uint32_t)ch[1] << 8) | (uint32_t)ch[2];
}

static gl_uniform* prog_find_uniform(gl_program* p, const char* name) {
    for(int i = 0; i < p->nuniform; ++i)
        if(!strcmp(p->uniforms[i]->name, name)) return p->uniforms[i];
    return NULL;
}
static gl_uniform* prog_add_uniform(gl_program* p, const char* name) {
    gl_uniform* u = prog_find_uniform(p, name);
    if(u) return u;
    if(p->nuniform == p->ucap) {
        p->ucap = p->ucap ? p->ucap * 2 : 8;
        p->uniforms = (gl_uniform**)realloc(p->uniforms, (size_t)p->ucap * sizeof(gl_uniform*));
    }
    u = (gl_uniform*)calloc(1, sizeof(gl_uniform));
    u->name = strdup(name);
    u->n = 1;
    p->uniforms[p->nuniform++] = u;
    return u;
}

/* ------------------------------------------------------------------ */
/* GLSL boundary callbacks                                            */
/* ------------------------------------------------------------------ */
static int nv_find(gl_nv* t, int n, const char* name, glsl_val* out) {
    for(int i = 0; i < n; ++i)
        if(t[i].name && !strcmp(t[i].name, name)) { *out = t[i].v; return 1; }
    return 0;
}
static void nv_set(gl_nv* t, int* n, const char* name, const glsl_val* v) {
    for(int i = 0; i < *n; ++i)
        if(t[i].name && !strcmp(t[i].name, name)) { t[i].v = *v; return; }
    if(*n < GL_MAX_NAMEVAL) { t[*n].name = name; t[*n].v = *v; (*n)++; }
}

/* Replace a pinned var slot (unref old, ref new; NULL clears). Callers must
 * run while the involved vars are valid - native methods or on_destroy. */
static void gl_pin(var_t** slot, var_t* v) {
    if(*slot == v) return;
    if(*slot != NULL) var_unref(*slot);
    *slot = v;
    if(v != NULL) var_ref(v);
}

/* A uniform location borrows the program-owned gl_uniform AND pins the
 * program var: a page may drop the program while keeping the location. */
typedef struct gl_uloc {
    gl_uniform* u;
    var_t*      prog;
} gl_uloc;
static void uloc_free(gl_uloc* l) { free(l); }
static void uloc_on_destroy(void* vp) {
    var_t* v = (var_t*)vp;
    gl_uloc* l = (v != NULL) ? (gl_uloc*)v->value : NULL;
    if(l != NULL && l->prog != NULL) { var_unref(l->prog); l->prog = NULL; }
}

/* Vertex/fragment input read: the per-invocation input table first, then the
 * program's uniform table. Built-in outputs are readable too (GLSL allows
 * `v_pos = gl_Position` and `gl_FragColor.rgb *= x` read-modify-write).
 * Unknown names get a zero value (interpreter does it). */
static int shade_getv(void* ud, const char* name, glsl_val* out) {
    shade_ud* s = (shade_ud*)ud;
    if(nv_find(s->in, s->nin, name, out)) return 1;
    if(s->out != NULL && !strcmp(name, "gl_Position")) {
        memset(out, 0, sizeof(*out));
        out->n = 4;
        for(int i = 0; i < 4; ++i) out->c[i] = s->out->clip[i];
        return 1;
    }
    if(s->out == NULL && !strcmp(name, "gl_FragColor")) {
        *out = s->fragcolor;
        if(out->n < 4) out->n = 4;
        return 1;
    }
    if(s->prog) {
        gl_uniform* u = prog_find_uniform(s->prog, name);
        if(u) {
            memset(out, 0, sizeof(*out));
            out->n = (u->n > 4) ? 4 : u->n;   /* c[] holds at most a vec4 */
            for(int i = 0; i < u->n && i < 4; ++i) out->c[i] = u->c[i];
            if(u->mat >= 2) {
                out->mat = u->mat; out->n = u->mat;
                for(int i = 0; i < u->mat * u->mat; ++i) out->m[i] = u->c[i];
            }
            if(u->samp) { out->samp = 1; out->n = 1; }
            return 1;
        }
    }
    return 0;
}

static void shade_setv(void* ud, const char* name, const glsl_val* v) {
    shade_ud* s = (shade_ud*)ud;
    if(s->out != NULL) {
        /* vertex stage */
        if(!strcmp(name, "gl_Position")) {
            for(int i = 0; i < 4; ++i) s->out->clip[i] = (i < v->n) ? v->c[i] : 0.0f;
            return;
        }
        if(!strcmp(name, "gl_PointSize")) { s->out->pointsize = v->c[0]; return; }
        nv_set(s->out->vary, &s->out->nvary, name, v);
        return;
    }
    /* fragment stage */
    if(!strcmp(name, "gl_FragColor")) { s->fragcolor = *v; s->have_frag = 1; }
}

/* texture2D() sampling: wrap + filter the RGBA8 store of the texture bound to
 * `unit`. Unbound / empty textures sample transparent black (WebGL spec).
 * Mipmap filters fall back to the base level (we keep a single level). */
static float wrap_coord(float x, int mode) {
    if(mode == GL_CLAMP_TO_EDGE) {
        if(x < 0.0f) return 0.0f;
        if(x > 1.0f) return 1.0f;
        return x;
    }
    if(mode == GL_MIRRORED_REPEAT) {
        float t = fabsf(x);
        int period = (int)floorf(t);
        float f = t - (float)period;
        return (period & 1) ? 1.0f - f : f;
    }
    /* GL_REPEAT */
    float f = x - floorf(x);
    return f;
}
static void tex_fetch(const gl_texture* t, int x, int y, float* rgba) {
    if(x < 0) x = 0; if(x >= t->w) x = t->w - 1;
    if(y < 0) y = 0; if(y >= t->h) y = t->h - 1;
    const uint8_t* p = t->rgba + ((size_t)y * t->w + x) * 4;
    rgba[0] = p[0] / 255.0f; rgba[1] = p[1] / 255.0f;
    rgba[2] = p[2] / 255.0f; rgba[3] = p[3] / 255.0f;
}
static int shade_sample(void* ud, int unit, float s, float t, glsl_val* out) {
    shade_ud* sd = (shade_ud*)ud;
    gl_ctx* g = sd->gl;
    memset(out, 0, sizeof(*out));
    out->n = 4;
    if(g == NULL || unit < 0 || unit >= GL_MAX_TEXTURE_UNITS) return 1;
    gl_texture* tx = g->tex_unit[unit];
    if(tx == NULL || tx->rgba == NULL || tx->w <= 0 || tx->h <= 0) return 1;
    float u = wrap_coord(s, tx->wrap_s) * tx->w - 0.5f;
    float v = wrap_coord(t, tx->wrap_t) * tx->h - 0.5f;
    int filter = tx->mag_filter;
    if(filter == GL_NEAREST) {
        float px[4];
        tex_fetch(tx, (int)floorf(u + 0.5f), (int)floorf(v + 0.5f), px);
        for(int i = 0; i < 4; ++i) out->c[i] = px[i];
        return 1;
    }
    /* LINEAR (and mipmap variants -> base level): bilinear of 4 texels. */
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fu = u - (float)x0, fv = v - (float)y0;
    float c00[4], c10[4], c01[4], c11[4];
    tex_fetch(tx, x0,     y0,     c00);
    tex_fetch(tx, x0 + 1, y0,     c10);
    tex_fetch(tx, x0,     y0 + 1, c01);
    tex_fetch(tx, x0 + 1, y0 + 1, c11);
    for(int i = 0; i < 4; ++i) {
        float a = c00[i] * (1.0f - fu) + c10[i] * fu;
        float b = c01[i] * (1.0f - fu) + c11[i] * fu;
        out->c[i] = a * (1.0f - fv) + b * fv;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Rasterizer                                                          */
/* ------------------------------------------------------------------ */
/* Run the fragment shader for one pixel with interpolated varyings and write
 * the resulting color into the framebuffer (respecting blend). */
static void shade_fragment(gl_ctx* g, shade_ud* base, gl_nv* vary, int nvary, int x, int y) {
    if(base->prog == NULL || base->prog->fs == NULL || base->prog->fs->prog == NULL) return;
    shade_ud s; memset(&s, 0, sizeof(s));
    s.gl = g; s.prog = base->prog; s.in = vary; s.nin = nvary; s.out = NULL;
    s.fragcolor.n = 4;   /* gl_FragColor reads before the first write see zero */
    int rc = glsl_run_ex(base->prog->fs->prog, &s, shade_getv, shade_setv, shade_sample);
    if(rc == 0 || !s.have_frag) return;   /* discard / no color written */
    uint32_t col = pack_rgba(s.fragcolor.c[0], s.fragcolor.c[1],
                             s.fragcolor.c[2], (s.fragcolor.n >= 4) ? s.fragcolor.c[3] : 1.0f);
    int tw = 0, th = 0;
    uint32_t* fb = gl_target(g, &tw, &th, NULL);
    if(x < 0 || y < 0 || x >= tw || y >= th) return;
    int idx = y * tw + x;
    if(g->blend_enabled) col = blend_srcover(fb[idx], col);
    fb[idx] = col;
    if(g->fbo == NULL) gl_mark_dirty(g, x, y);
}

/* Interpolate a varying table across barycentric weights. */
static void interp_vary(gl_vertout* a, gl_vertout* b, gl_vertout* c,
                        float wa, float wb, float wc, gl_nv* out, int* nout) {
    *nout = 0;
    for(int i = 0; i < a->nvary; ++i) {
        const char* nm = a->vary[i].name;
        glsl_val av = a->vary[i].v;
        glsl_val bv = {0}, cv = {0};
        bv.n = cv.n = av.n;
        nv_find(b->vary, b->nvary, nm, &bv);
        nv_find(c->vary, c->nvary, nm, &cv);
        glsl_val r; memset(&r, 0, sizeof(r)); r.n = av.n;
        for(int k = 0; k < av.n; ++k)
            r.c[k] = wa * av.c[k] + wb * bv.c[k] + wc * cv.c[k];
        nv_set(out, nout, nm, &r);
    }
}

static void raster_triangle(gl_ctx* g, shade_ud* base, gl_vertout* v0, gl_vertout* v1, gl_vertout* v2) {
    float w0 = v0->clip[3], w1 = v1->clip[3], w2 = v2->clip[3];
    if(w0 == 0) w0 = 1; if(w1 == 0) w1 = 1; if(w2 == 0) w2 = 1;
    /* perspective divide -> NDC -> viewport (flip Y: framebuffer row 0 = top) */
    float sx0 = g->vp_x + (v0->clip[0] / w0 * 0.5f + 0.5f) * g->vp_w;
    float sy0 = g->vp_y + (1.0f - (v0->clip[1] / w0 * 0.5f + 0.5f)) * g->vp_h;
    float sx1 = g->vp_x + (v1->clip[0] / w1 * 0.5f + 0.5f) * g->vp_w;
    float sy1 = g->vp_y + (1.0f - (v1->clip[1] / w1 * 0.5f + 0.5f)) * g->vp_h;
    float sx2 = g->vp_x + (v2->clip[0] / w2 * 0.5f + 0.5f) * g->vp_w;
    float sy2 = g->vp_y + (1.0f - (v2->clip[1] / w2 * 0.5f + 0.5f)) * g->vp_h;

    float minx = sx0 < sx1 ? (sx0 < sx2 ? sx0 : sx2) : (sx1 < sx2 ? sx1 : sx2);
    float maxx = sx0 > sx1 ? (sx0 > sx2 ? sx0 : sx2) : (sx1 > sx2 ? sx1 : sx2);
    float miny = sy0 < sy1 ? (sy0 < sy2 ? sy0 : sy2) : (sy1 < sy2 ? sy1 : sy2);
    float maxy = sy0 > sy1 ? (sy0 > sy2 ? sy0 : sy2) : (sy1 > sy2 ? sy1 : sy2);
    int x0i = (int)floorf(minx), x1i = (int)ceilf(maxx);
    int y0i = (int)floorf(miny), y1i = (int)ceilf(maxy);
    int tw = 0, th = 0;
    gl_target(g, &tw, &th, NULL);   /* clip against the active render target */
    if(x0i < 0) x0i = 0; if(y0i < 0) y0i = 0;
    if(x1i > tw - 1) x1i = tw - 1; if(y1i > th - 1) y1i = th - 1;

    float denom = (sy1 - sy2) * (sx0 - sx2) + (sx2 - sx1) * (sy0 - sy2);
    if(fabsf(denom) < 1e-20f) return;   /* degenerate */
    float inv = 1.0f / denom;

    gl_nv vary[GL_MAX_NAMEVAL];
    for(int py = y0i; py <= y1i; ++py) {
        for(int px = x0i; px <= x1i; ++px) {
            float pcx = px + 0.5f, pcy = py + 0.5f;
            float wa = ((sy1 - sy2) * (pcx - sx2) + (sx2 - sx1) * (pcy - sy2)) * inv;
            float wb = ((sy2 - sy0) * (pcx - sx2) + (sx0 - sx2) * (pcy - sy2)) * inv;
            float wc = 1.0f - wa - wb;
            if(wa < 0 || wb < 0 || wc < 0) continue;
            /* back-face culling (screen-space signed area) */
            if(g->cull_enabled) {
                float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
                int ccw = area > 0;
                int front = (g->front_face == GL_CCW) ? ccw : !ccw;
                if((g->cull_mode == GL_BACK && !front) ||
                   (g->cull_mode == GL_FRONT && front)) continue;
            }
            int nv = 0;
            interp_vary(v0, v1, v2, wa, wb, wc, vary, &nv);
            shade_fragment(g, base, vary, nv, px, py);
        }
    }
}

static void raster_line(gl_ctx* g, shade_ud* base, gl_vertout* a, gl_vertout* b) {
    float wa = a->clip[3] ? a->clip[3] : 1, wb = b->clip[3] ? b->clip[3] : 1;
    float ax = g->vp_x + (a->clip[0] / wa * 0.5f + 0.5f) * g->vp_w;
    float ay = g->vp_y + (1.0f - (a->clip[1] / wa * 0.5f + 0.5f)) * g->vp_h;
    float bx = g->vp_x + (b->clip[0] / wb * 0.5f + 0.5f) * g->vp_w;
    float by = g->vp_y + (1.0f - (b->clip[1] / wb * 0.5f + 0.5f)) * g->vp_h;
    int steps = (int)(fabsf(bx - ax) > fabsf(by - ay) ? fabsf(bx - ax) : fabsf(by - ay));
    if(steps < 1) steps = 1;
    if(steps > 4096) steps = 4096;
    gl_nv vary[GL_MAX_NAMEVAL];
    for(int i = 0; i <= steps; ++i) {
        float t = (float)i / (float)steps;
        int px = (int)(ax + (bx - ax) * t + 0.5f);
        int py = (int)(ay + (by - ay) * t + 0.5f);
        int nv = 0;
        interp_vary(a, b, b, 1.0f - t, t, 0.0f, vary, &nv);
        shade_fragment(g, base, vary, nv, px, py);
    }
}

static void raster_point(gl_ctx* g, shade_ud* base, gl_vertout* v) {
    float wv = v->clip[3] ? v->clip[3] : 1;
    float sx = g->vp_x + (v->clip[0] / wv * 0.5f + 0.5f) * g->vp_w;
    float sy = g->vp_y + (1.0f - (v->clip[1] / wv * 0.5f + 0.5f)) * g->vp_h;
    int r = (int)(v->pointsize * 0.5f); if(r < 0) r = 0;
    int cx = (int)(sx + 0.5f), cy = (int)(sy + 0.5f);
    for(int dy = -r; dy <= r; ++dy)
        for(int dx = -r; dx <= r; ++dx)
            shade_fragment(g, base, v->vary, v->nvary, cx + dx, cy + dy);
}
/* ------------------------------------------------------------------ */
/* Vertex fetch + drawArrays                                           */
/* ------------------------------------------------------------------ */
static gl_ctx* this_ctx(var_t* env) {
    var_t* t = js_this(env);
    return (t != NULL) ? (gl_ctx*)t->value : NULL;
}

/* A plain JS object with Object.prototype (the canonical native idiom). */
static var_t* plain_obj(vm_t* vm) {
    return var_new_obj(vm, var_get_prototype(vm->builtin_vars.var_Object), NULL, NULL);
}

/* Run the vertex shader for logical vertex `idx`, filling `out`. Attribute
 * values are read from each enabled attrib slot's bound ARRAY_BUFFER using the
 * vertexAttribPointer layout (FLOAT path; stride/offset are in bytes). */
static void fetch_vertex(gl_ctx* g, gl_program* p, int idx, gl_vertout* out) {
    memset(out, 0, sizeof(*out));
    out->clip[3] = 1.0f; out->pointsize = 1.0f;
    gl_nv in[GL_MAX_NAMEVAL]; int nin = 0;
    for(int loc = 0; loc < GL_MAX_ATTRIBS; ++loc) {
        gl_attrib* at = &g->attrib[loc];
        if(!at->enabled || at->buffer == NULL) continue;
        if(loc >= p->nattr || p->attr_name[loc] == NULL) continue;
        int fstride = at->stride ? at->stride / 4 : at->size;   /* floats per vertex */
        int foff = at->offset / 4;
        glsl_val v; memset(&v, 0, sizeof(v)); v.n = at->size;
        for(int c = 0; c < at->size && c < 4; ++c) {
            int fi = foff + idx * fstride + c;
            v.c[c] = (fi >= 0 && fi < at->buffer->nf) ? at->buffer->fdata[fi] : 0.0f;
        }
        for(int c = at->size; c < 4; ++c) v.c[c] = 0.0f;
        nv_set(in, &nin, p->attr_name[loc], &v);
    }
    shade_ud s; memset(&s, 0, sizeof(s));
    s.gl = g; s.prog = p; s.in = in; s.nin = nin; s.out = out;
    if(p->vs && p->vs->prog) glsl_run_ex(p->vs->prog, &s, shade_getv, shade_setv, shade_sample);
}

static var_t* gl_drawArrays(vm_t* vm, var_t* env, void* data) {
    (void)data;
    gl_ctx* g = this_ctx(env);
    int mode = js_arg_int(env, 0), first = js_arg_int(env, 1), count = js_arg_int(env, 2);
    if(g == NULL || g->program == NULL || !g->program->link_status) return NULL;
    if(count <= 0) return NULL;
    if(first < 0) first = 0;
    if(count > GL_MAX_VERTS) count = GL_MAX_VERTS;
    gl_vertout* v = (gl_vertout*)calloc((size_t)count, sizeof(gl_vertout));
    if(v == NULL) return NULL;
    for(int i = 0; i < count; ++i) fetch_vertex(g, g->program, first + i, &v[i]);
    shade_ud base; memset(&base, 0, sizeof(base));
    base.gl = g; base.prog = g->program;
    switch(mode) {
        case GL_POINTS:
            for(int i = 0; i < count; ++i) raster_point(g, &base, &v[i]);
            break;
        case GL_LINES:
            for(int i = 0; i + 1 < count; i += 2) raster_line(g, &base, &v[i], &v[i+1]);
            break;
        case GL_LINE_STRIP:
            for(int i = 0; i + 1 < count; ++i) raster_line(g, &base, &v[i], &v[i+1]);
            break;
        case GL_LINE_LOOP:
            for(int i = 0; i < count; ++i) raster_line(g, &base, &v[i], &v[(i+1)%count]);
            break;
        case GL_TRIANGLES:
            for(int i = 0; i + 2 < count; i += 3) raster_triangle(g, &base, &v[i], &v[i+1], &v[i+2]);
            break;
        case GL_TRIANGLE_STRIP:
            for(int i = 0; i + 2 < count; ++i) {
                if(i & 1) raster_triangle(g, &base, &v[i+1], &v[i], &v[i+2]);
                else      raster_triangle(g, &base, &v[i], &v[i+1], &v[i+2]);
            }
            break;
        case GL_TRIANGLE_FAN:
            for(int i = 1; i + 1 < count; ++i) raster_triangle(g, &base, &v[0], &v[i], &v[i+1]);
            break;
        default: break;
    }
    free(v);
    gl_flush(g);
    (void)vm;
    return NULL;   /* void */
}

static var_t* gl_drawElements(vm_t* vm, var_t* env, void* data) {
    (void)data;
    gl_ctx* g = this_ctx(env);
    int mode = js_arg_int(env, 0), count = js_arg_int(env, 1);
    int ioff = js_arg_int(env, 3);   /* byte offset into the element buffer */
    if(g == NULL || g->program == NULL || !g->program->link_status) return NULL;
    gl_buffer* eb = g->element_buffer;
    if(eb == NULL || eb->fdata == NULL || count <= 0) return NULL;
    if(count > GL_MAX_VERTS) count = GL_MAX_VERTS;
    int start = ioff / 4;
    gl_vertout* v = (gl_vertout*)calloc((size_t)count, sizeof(gl_vertout));
    if(v == NULL) return NULL;
    for(int i = 0; i < count; ++i) {
        int ei = start + i;
        int idx = (ei >= 0 && ei < eb->nf) ? (int)eb->fdata[ei] : 0;
        fetch_vertex(g, g->program, idx, &v[i]);
    }
    shade_ud base; memset(&base, 0, sizeof(base));
    base.gl = g; base.prog = g->program;
    switch(mode) {
        case GL_TRIANGLES:
            for(int i = 0; i + 2 < count; i += 3) raster_triangle(g, &base, &v[i], &v[i+1], &v[i+2]);
            break;
        case GL_TRIANGLE_STRIP:
            for(int i = 0; i + 2 < count; ++i) {
                if(i & 1) raster_triangle(g, &base, &v[i+1], &v[i], &v[i+2]);
                else      raster_triangle(g, &base, &v[i], &v[i+1], &v[i+2]);
            }
            break;
        case GL_TRIANGLE_FAN:
            for(int i = 1; i + 1 < count; ++i) raster_triangle(g, &base, &v[0], &v[i], &v[i+1]);
            break;
        case GL_LINES:
            for(int i = 0; i + 1 < count; i += 2) raster_line(g, &base, &v[i], &v[i+1]);
            break;
        case GL_LINE_STRIP:
            for(int i = 0; i + 1 < count; ++i) raster_line(g, &base, &v[i], &v[i+1]);
            break;
        case GL_POINTS:
            for(int i = 0; i < count; ++i) raster_point(g, &base, &v[i]);
            break;
        default: break;
    }
    free(v);
    gl_flush(g);
    (void)vm;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Buffer natives                                                      */
/* ------------------------------------------------------------------ */
static void buffer_free(gl_buffer* b) {
    if(b == NULL) return;
    free(b->fdata); free(b);
}
static var_t* gl_createBuffer(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    gl_buffer* b = (gl_buffer*)calloc(1, sizeof(gl_buffer));
    if(b == NULL) return var_new_null(vm);
    return var_new_obj(vm, g_buf_proto, b, (free_func_t)buffer_free);
}
static var_t* gl_bindBuffer(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int target = js_arg_int(env, 0);
    var_t* bv = js_arg(env, 1);
    gl_buffer* b = (bv != NULL && bv->type == V_OBJECT) ? (gl_buffer*)bv->value : NULL;
    if(target == GL_ELEMENT_ARRAY_BUFFER) {
        g->element_buffer = b; gl_pin(&g->element_buffer_var, b ? bv : NULL);
    } else {
        g->array_buffer = b; gl_pin(&g->array_buffer_var, b ? bv : NULL);
    }
    return NULL;
}
static var_t* gl_bufferData(vm_t* vm, var_t* env, void* data) {
    (void)data;
    gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int target = js_arg_int(env, 0);
    var_t* d = js_arg(env, 1);
    gl_buffer* b = (target == GL_ELEMENT_ARRAY_BUFFER) ? g->element_buffer : g->array_buffer;
    if(b == NULL) return NULL;
    b->usage = js_arg_int(env, 2);
    free(b->fdata); b->fdata = NULL; b->nf = 0;
    if(d == NULL) return NULL;
    if(var_is_typedarray(d) || d->is_array) {
        int n = var_is_typedarray(d) ? var_get_int(var_find_own_member_var(d, "length"))
                                     : (int)var_array_size(d);
        if(n < 0) n = 0;
        b->fdata = (float*)calloc((size_t)(n > 0 ? n : 1), sizeof(float));
        b->nf = n;
        for(int i = 0; i < n; ++i) {
            var_t* e;
            if(var_is_typedarray(d)) { e = var_typedarray_get_at(vm, d, i); b->fdata[i] = (float)js_num(e); if(e) var_unref(e); }
            else { node_t* nd = var_array_get(d, i); e = (nd != NULL) ? nd->var : NULL; b->fdata[i] = (float)js_num(e); }
        }
    } else {
        int sz = (int)js_num(d);
        if(sz < 0) sz = 0;
        b->fdata = (float*)calloc((size_t)(sz > 0 ? sz : 1), sizeof(float));
        b->nf = sz;   /* zero-filled */
    }
    return NULL;
}
static var_t* gl_deleteBuffer(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    gl_ctx* g = this_ctx(env);
    var_t* bv = js_arg(env, 0);
    gl_buffer* b = (bv != NULL && bv->type == V_OBJECT) ? (gl_buffer*)bv->value : NULL;
    if(g == NULL || b == NULL) return NULL;
    if(g->array_buffer == b) { g->array_buffer = NULL; gl_pin(&g->array_buffer_var, NULL); }
    if(g->element_buffer == b) { g->element_buffer = NULL; gl_pin(&g->element_buffer_var, NULL); }
    for(int i = 0; i < GL_MAX_ATTRIBS; ++i)
        if(g->attrib[i].buffer == b) { g->attrib[i].buffer = NULL; gl_pin(&g->attrib[i].buffer_var, NULL); }
    return NULL;   /* real free happens when the JS object is collected */
}

/* ------------------------------------------------------------------ */
/* Shader natives                                                      */
/* ------------------------------------------------------------------ */
static void shader_free(gl_shader* s);
static var_t* gl_createShader(vm_t* vm, var_t* env, void* data) {
    (void)data;
    int type = js_arg_int(env, 0);
    gl_shader* s = (gl_shader*)calloc(1, sizeof(gl_shader));
    if(s == NULL) return var_new_null(vm);
    s->type = type;
    return var_new_obj(vm, g_shd_proto, s, (free_func_t)shader_free);
}
static void shader_free(gl_shader* s) {
    if(s == NULL) return;
    free(s->source); free(s->info_log);
    if(s->prog) glsl_free(s->prog);
    free(s);
}
static var_t* gl_shaderSource(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* sv = js_arg(env, 0);
    gl_shader* s = (sv != NULL && sv->type == V_OBJECT) ? (gl_shader*)sv->value : NULL;
    if(s == NULL) return NULL;
    mstr_t* m = mstr_new("");
    const char* src = js_cstr(js_arg(env, 1), m);
    free(s->source); s->source = strdup(src ? src : "");
    mstr_free(m);
    return NULL;
}
static var_t* gl_compileShader(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* sv = js_arg(env, 0);
    gl_shader* s = (sv != NULL && sv->type == V_OBJECT) ? (gl_shader*)sv->value : NULL;
    if(s == NULL) return NULL;
    if(s->prog) { glsl_free(s->prog); s->prog = NULL; }
    free(s->info_log); s->info_log = NULL;
    int stage = (s->type == GL_VERTEX_SHADER) ? GLSL_STAGE_VERTEX : GLSL_STAGE_FRAGMENT;
    char* err = NULL;
    s->prog = glsl_compile(s->source ? s->source : "", stage, &err);
    s->compile_status = (s->prog != NULL) ? 1 : 0;
    if(err) { s->info_log = err; }
    return NULL;
}
static var_t* gl_getShaderParameter(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* sv = js_arg(env, 0);
    gl_shader* s = (sv != NULL && sv->type == V_OBJECT) ? (gl_shader*)sv->value : NULL;
    int pname = js_arg_int(env, 1);
    if(s == NULL) return var_new_bool(vm, false);
    if(pname == GL_COMPILE_STATUS) return var_new_bool(vm, s->compile_status != 0);
    if(pname == GL_SHADER_TYPE)    return var_new_int(vm, s->type);
    if(pname == GL_INFO_LOG_LENGTH) return var_new_int(vm, s->info_log ? (int)strlen(s->info_log) : 0);
    if(pname == GL_DELETE_STATUS)  return var_new_bool(vm, s->deleted != 0);
    return var_new_bool(vm, false);
}
static var_t* gl_getShaderInfoLog(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* sv = js_arg(env, 0);
    gl_shader* s = (sv != NULL && sv->type == V_OBJECT) ? (gl_shader*)sv->value : NULL;
    return var_new_str(vm, (s && s->info_log) ? s->info_log : "");
}
static var_t* gl_deleteShader(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* sv = js_arg(env, 0);
    gl_shader* s = (sv != NULL && sv->type == V_OBJECT) ? (gl_shader*)sv->value : NULL;
    if(s) s->deleted = 1;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Program natives                                                     */
/* ------------------------------------------------------------------ */
static void program_free(gl_program* p) {
    if(p == NULL) return;
    free(p->info_log);
    for(int i = 0; i < p->nattr; ++i) free(p->attr_name[i]);
    for(int i = 0; i < p->nuniform; ++i) { free(p->uniforms[i]->name); free(p->uniforms[i]); }
    free(p->uniforms);
    free(p);
}
static void program_on_destroy(void* vp) {
    var_t* v = (var_t*)vp;
    gl_program* p = (v != NULL) ? (gl_program*)v->value : NULL;
    if(p == NULL) return;
    gl_pin(&p->vs_var, NULL);
    gl_pin(&p->fs_var, NULL);
}
static var_t* gl_createProgram(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    gl_program* p = (gl_program*)calloc(1, sizeof(gl_program));
    if(p == NULL) return var_new_null(vm);
    var_t* obj = var_new_obj(vm, g_prg_proto, p, (free_func_t)program_free);
    if(obj == NULL) { program_free(p); return var_new_null(vm); }
    obj->on_destroy = program_on_destroy;
    return obj;
}
static var_t* gl_attachShader(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* pv = js_arg(env, 0), *sv = js_arg(env, 1);
    gl_program* p = (pv != NULL && pv->type == V_OBJECT) ? (gl_program*)pv->value : NULL;
    gl_shader* s  = (sv != NULL && sv->type == V_OBJECT) ? (gl_shader*)sv->value : NULL;
    if(p == NULL || s == NULL) return NULL;
    if(s->type == GL_VERTEX_SHADER) { p->vs = s; gl_pin(&p->vs_var, sv); }
    else                            { p->fs = s; gl_pin(&p->fs_var, sv); }
    return NULL;
}
static var_t* gl_linkProgram(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    var_t* pv = js_arg(env, 0);
    gl_program* p = (pv != NULL && pv->type == V_OBJECT) ? (gl_program*)pv->value : NULL;
    if(p == NULL) return NULL;
    free(p->info_log); p->info_log = NULL;
    if(p->vs == NULL || p->fs == NULL || p->vs->prog == NULL || p->fs->prog == NULL) {
        p->link_status = 0;
        p->info_log = strdup("missing or uncompilable vertex/fragment shader");
        return NULL;
    }
    p->link_status = 1;
    /* attribute reflection from the vertex shader -> locations 0..n-1 */
    for(int i = 0; i < p->nattr; ++i) { free(p->attr_name[i]); p->attr_name[i] = NULL; }
    p->nattr = 0;
    int na = glsl_var_count(p->vs->prog, GLSL_VAR_ATTRIB);
    for(int i = 0; i < na && i < GL_MAX_ATTRIBS; ++i)
        p->attr_name[p->nattr++] = strdup(glsl_var_name(p->vs->prog, GLSL_VAR_ATTRIB, i));
    /* uniform reflection from both stages (shape included: matN / sampler2D) */
    for(int i = 0; i < p->nuniform; ++i) { free(p->uniforms[i]->name); free(p->uniforms[i]); }
    p->nuniform = 0;
    for(int st = 0; st < 2; ++st) {
        glsl_shader* sh = st ? p->fs->prog : p->vs->prog;
        int nu = glsl_var_count(sh, GLSL_VAR_UNIFORM);
        for(int i = 0; i < nu; ++i) {
            gl_uniform* u = prog_add_uniform(p, glsl_var_name(sh, GLSL_VAR_UNIFORM, i));
            u->mat = glsl_var_mat(sh, GLSL_VAR_UNIFORM, i);
            u->samp = glsl_var_sampler(sh, GLSL_VAR_UNIFORM, i);
            if(u->mat >= 2) u->n = u->mat * u->mat;
            /* Samplers default to texture unit 0 (GL spec); uniform1i retargets. */
            if(u->samp) { u->n = 1; u->c[0] = 0.0f; }
        }
    }
    return NULL;
}
static var_t* gl_useProgram(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    gl_ctx* g = this_ctx(env);
    var_t* pv = js_arg(env, 0);
    gl_program* p = (pv != NULL && pv->type == V_OBJECT) ? (gl_program*)pv->value : NULL;
    if(g) { g->program = p; gl_pin(&g->program_var, p ? pv : NULL); }
    return NULL;
}
static var_t* gl_getProgramParameter(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* pv = js_arg(env, 0);
    gl_program* p = (pv != NULL && pv->type == V_OBJECT) ? (gl_program*)pv->value : NULL;
    int pname = js_arg_int(env, 1);
    if(p == NULL) return var_new_bool(vm, false);
    if(pname == GL_LINK_STATUS)     return var_new_bool(vm, p->link_status != 0);
    if(pname == GL_VALIDATE_STATUS) return var_new_bool(vm, p->link_status != 0);
    if(pname == GL_DELETE_STATUS)   return var_new_bool(vm, p->deleted != 0);
    if(pname == GL_INFO_LOG_LENGTH) return var_new_int(vm, p->info_log ? (int)strlen(p->info_log) : 0);
    return var_new_int(vm, 0);
}
static var_t* gl_getProgramInfoLog(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* pv = js_arg(env, 0);
    gl_program* p = (pv != NULL && pv->type == V_OBJECT) ? (gl_program*)pv->value : NULL;
    return var_new_str(vm, (p && p->info_log) ? p->info_log : "");
}
static var_t* gl_deleteProgram(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data;
    gl_ctx* g = this_ctx(env);
    var_t* pv = js_arg(env, 0);
    gl_program* p = (pv != NULL && pv->type == V_OBJECT) ? (gl_program*)pv->value : NULL;
    if(p) p->deleted = 1;
    if(g && g->program == p) { g->program = NULL; gl_pin(&g->program_var, NULL); }
    return NULL;
}
static var_t* gl_getAttribLocation(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* pv = js_arg(env, 0);
    gl_program* p = (pv != NULL && pv->type == V_OBJECT) ? (gl_program*)pv->value : NULL;
    mstr_t* m = mstr_new("");
    const char* name = js_cstr(js_arg(env, 1), m);
    int loc = -1;
    if(p) for(int i = 0; i < p->nattr; ++i) if(!strcmp(p->attr_name[i], name)) { loc = i; break; }
    mstr_free(m);
    return var_new_int(vm, loc);
}
static var_t* gl_getUniformLocation(vm_t* vm, var_t* env, void* data) {
    (void)data;
    var_t* pv = js_arg(env, 0);
    gl_program* p = (pv != NULL && pv->type == V_OBJECT) ? (gl_program*)pv->value : NULL;
    mstr_t* m = mstr_new("");
    const char* name = js_cstr(js_arg(env, 1), m);
    gl_uniform* u = p ? prog_find_uniform(p, name) : NULL;
    mstr_free(m);
    if(u == NULL) return var_new_null(vm);
    /* The location object borrows the program-owned gl_uniform: it must NOT
     * free the payload (a NULL free_func makes var_clean() mario_free() it,
     * freeing the uniform out from under the program). The wrapper pins the
     * program var so the uniform table outlives the location. */
    gl_uloc* l = (gl_uloc*)calloc(1, sizeof(gl_uloc));
    if(l == NULL) return var_new_null(vm);
    l->u = u; l->prog = var_ref(pv);
    var_t* obj = var_new_obj(vm, g_uloc_proto, l, (free_func_t)uloc_free);
    if(obj == NULL) { var_unref(pv); free(l); return var_new_null(vm); }
    obj->on_destroy = uloc_on_destroy;
    return obj;
}

/* ------------------------------------------------------------------ */
/* Uniform + attribute natives                                         */
/* ------------------------------------------------------------------ */
static gl_uniform* loc_uniform(var_t* env, int argi) {
    var_t* lv = js_arg(env, argi);
    gl_uloc* l = (lv != NULL && lv->type == V_OBJECT) ? (gl_uloc*)lv->value : NULL;
    return (l != NULL) ? l->u : NULL;
}
static var_t* gl_uniform1f(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_uniform* u = loc_uniform(env, 0);
    if(u) { u->n = 1; u->c[0] = (float)js_arg_num(env, 1); }
    return NULL;
}
static var_t* gl_uniform1i(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_uniform* u = loc_uniform(env, 0);
    if(u) { u->n = 1; u->c[0] = (float)js_arg_int(env, 1); }
    return NULL;
}
static var_t* gl_uniform2f(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_uniform* u = loc_uniform(env, 0);
    if(u) { u->n = 2; u->c[0] = (float)js_arg_num(env, 1); u->c[1] = (float)js_arg_num(env, 2); }
    return NULL;
}
static var_t* gl_uniform2i(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_uniform* u = loc_uniform(env, 0);
    if(u) { u->n = 2; u->c[0] = (float)js_arg_int(env, 1); u->c[1] = (float)js_arg_int(env, 2); }
    return NULL;
}
static var_t* gl_uniform3f(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_uniform* u = loc_uniform(env, 0);
    if(u) { u->n = 3; for(int i = 0; i < 3; ++i) u->c[i] = (float)js_arg_num(env, 1 + i); }
    return NULL;
}
static var_t* gl_uniform4f(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_uniform* u = loc_uniform(env, 0);
    if(u) { u->n = 4; for(int i = 0; i < 4; ++i) u->c[i] = (float)js_arg_num(env, 1 + i); }
    return NULL;
}
/* uniformMatrixNfv(loc, transpose, value): stores the matrix column-major and
 * shading applies real matN algebra (js_glsl). WebGL requires transpose=false;
 * a true flag is ignored (the values are taken as given, never transposed). */
static var_t* gl_uniformMatrixNfv(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_uniform* u = loc_uniform(env, 0);
    if(u == NULL) return NULL;
    var_t* arr = js_arg(env, 2);
    if(arr == NULL) return NULL;
    int n = var_is_typedarray(arr) ? var_get_int(var_find_own_member_var(arr, "length"))
                                   : (arr->is_array ? (int)var_array_size(arr) : 0);
    if(n > 16) n = 16;
    u->n = n;
    for(int i = 0; i < n; ++i) {
        var_t* e;
        if(var_is_typedarray(arr)) { e = var_typedarray_get_at(vm, arr, i); u->c[i] = (float)js_num(e); if(e) var_unref(e); }
        else { node_t* nd = var_array_get(arr, i); e = (nd != NULL) ? nd->var : NULL; u->c[i] = (float)js_num(e); }
    }
    /* Derive the matrix shape from the element count when reflection did not
     * supply one (4 -> mat2, 9 -> mat3, 16 -> mat4). */
    if(u->mat == 0) {
        if(n == 4) u->mat = 2;
        else if(n == 9) u->mat = 3;
        else if(n == 16) u->mat = 4;
    }
    return NULL;
}
/* uniformNfv / uniform1iv: flat float arrays (vecN arrays, sampler indices). */
static var_t* gl_uniformNfv(vm_t* vm, var_t* env, void* data) {
    (void)data; gl_uniform* u = loc_uniform(env, 0);
    if(u == NULL) return NULL;
    var_t* arr = js_arg(env, 1);
    if(arr == NULL) return NULL;
    int n = var_is_typedarray(arr) ? var_get_int(var_find_own_member_var(arr, "length"))
                                   : (arr->is_array ? (int)var_array_size(arr) : 0);
    if(n > 16) n = 16;
    if(n < 1) n = 1;
    u->n = n;
    for(int i = 0; i < n; ++i) {
        var_t* e;
        if(var_is_typedarray(arr)) { e = var_typedarray_get_at(vm, arr, i); u->c[i] = (float)js_num(e); if(e) var_unref(e); }
        else { node_t* nd = var_array_get(arr, i); e = (nd != NULL) ? nd->var : NULL; u->c[i] = (float)js_num(e); }
    }
    (void)vm;
    return NULL;
}
static var_t* gl_enableVertexAttribArray(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    int loc = js_arg_int(env, 0);
    if(g && loc >= 0 && loc < GL_MAX_ATTRIBS) g->attrib[loc].enabled = 1;
    return NULL;
}
static var_t* gl_disableVertexAttribArray(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    int loc = js_arg_int(env, 0);
    if(g && loc >= 0 && loc < GL_MAX_ATTRIBS) g->attrib[loc].enabled = 0;
    return NULL;
}
static var_t* gl_vertexAttribPointer(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    int loc = js_arg_int(env, 0);
    if(g == NULL || loc < 0 || loc >= GL_MAX_ATTRIBS) return NULL;
    gl_attrib* at = &g->attrib[loc];
    at->buffer = g->array_buffer;
    gl_pin(&at->buffer_var, g->array_buffer_var);
    at->size = js_arg_int(env, 1);
    at->type = js_arg_int(env, 2);
    at->normalized = js_truthy(js_arg(env, 3)) ? 1 : 0;
    at->stride = js_arg_int(env, 4);
    at->offset = js_arg_int(env, 5);
    if(at->size < 1) at->size = 1; if(at->size > 4) at->size = 4;
    return NULL;
}
/* ------------------------------------------------------------------ */
/* Texture natives                                                     */
/* ------------------------------------------------------------------ */
static void texture_free(gl_texture* t) {
    if(t == NULL) return;
    free(t->rgba); free(t);
}
static var_t* gl_createTexture(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    gl_texture* t = (gl_texture*)calloc(1, sizeof(gl_texture));
    if(t == NULL) return var_new_null(vm);
    /* GL defaults: min NEAREST_MIPMAP_LINEAR, mag LINEAR, wrap REPEAT. We keep
     * a single mip level, so mipmap filters sample the base level. */
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
    return var_new_obj(vm, g_tex_proto, t, (free_func_t)texture_free);
}
static gl_texture* arg_texture(var_t* env, int idx) {
    var_t* tv = js_arg(env, idx);
    return (tv != NULL && tv->type == V_OBJECT) ? (gl_texture*)tv->value : NULL;
}
static var_t* gl_activeTexture(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int unit = js_arg_int(env, 0) - GL_TEXTURE0;
    if(unit >= 0 && unit < GL_MAX_TEXTURE_UNITS) g->active_unit = unit;
    return NULL;
}
static var_t* gl_bindTexture(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int target = js_arg_int(env, 0);
    if(target != GL_TEXTURE_2D) return NULL;
    var_t* tv = js_arg(env, 1);
    gl_texture* t = arg_texture(env, 1);
    g->tex_unit[g->active_unit] = t;   /* NULL unbinds */
    gl_pin(&g->tex_unit_var[g->active_unit], t ? tv : NULL);
    return NULL;
}
static var_t* gl_texParameteri(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    gl_texture* t = g->tex_unit[g->active_unit];
    if(t == NULL) return NULL;
    int pname = js_arg_int(env, 1), param = js_arg_int(env, 2);
    if(pname == GL_TEXTURE_MIN_FILTER) t->min_filter = param;
    else if(pname == GL_TEXTURE_MAG_FILTER) t->mag_filter = param;
    else if(pname == GL_TEXTURE_WRAP_S) t->wrap_s = param;
    else if(pname == GL_TEXTURE_WRAP_T) t->wrap_t = param;
    return NULL;
}
static var_t* gl_pixelStorei(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int pname = js_arg_int(env, 0), param = js_arg_int(env, 1);
    if(pname == GL_UNPACK_FLIP_Y_WEBGL) g->unpack_flip_y = param ? 1 : 0;
    else if(pname == GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL) g->unpack_premult = param ? 1 : 0;
    else if(pname == GL_UNPACK_ALIGNMENT) {
        if(param == 1 || param == 2 || param == 4 || param == 8) g->unpack_alignment = param;
    }
    return NULL;
}
static var_t* gl_deleteTexture(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    gl_texture* t = arg_texture(env, 0);
    if(g == NULL || t == NULL) return NULL;
    t->deleted = 1;
    for(int i = 0; i < GL_MAX_TEXTURE_UNITS; ++i)
        if(g->tex_unit[i] == t) { g->tex_unit[i] = NULL; gl_pin(&g->tex_unit_var[i], NULL); }
    /* A deleted texture also detaches from any FBO color attachment. */
    if(g->fbo != NULL && g->fbo->color == t) {
        g->fbo->color = NULL;
        gl_pin(&g->fbo->color_var, NULL);
    }
    return NULL;   /* real free happens when the JS object is collected */
}

/* ------------------------------------------------------------------ */
/* Framebuffer / renderbuffer natives                                  */
/* ------------------------------------------------------------------ */
static void fbo_free(gl_fbo* f) {
    if(f == NULL) return;
    free(f->fb); free(f->depth); free(f);
}
/* The attached texture var is pinned (the fbo borrows its rgba store), so
 * the ref must drop in on_destroy while the var is still valid. */
static void fbo_on_destroy(void* vp) {
    var_t* v = (var_t*)vp;
    if(v == NULL) return;
    gl_fbo* f = (gl_fbo*)v->value;
    if(f == NULL) return;
    gl_pin(&f->color_var, NULL);
}
static var_t* gl_createFramebuffer(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    gl_fbo* f = (gl_fbo*)calloc(1, sizeof(gl_fbo));
    if(f == NULL) return var_new_null(vm);
    var_t* obj = var_new_obj(vm, g_fbo_proto, f, (free_func_t)fbo_free);
    if(obj == NULL) { fbo_free(f); return var_new_null(vm); }
    obj->on_destroy = fbo_on_destroy;
    return obj;
}
static gl_fbo* arg_fbo(var_t* env, int idx) {
    var_t* fv = js_arg(env, idx);
    return (fv != NULL && fv->type == V_OBJECT) ? (gl_fbo*)fv->value : NULL;
}
static gl_rbo* arg_rbo(var_t* env, int idx) {
    var_t* rv = js_arg(env, idx);
    return (rv != NULL && rv->type == V_OBJECT) ? (gl_rbo*)rv->value : NULL;
}
static var_t* gl_bindFramebuffer(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL || js_arg_int(env, 0) != GL_FRAMEBUFFER) return NULL;
    var_t* fv = js_arg(env, 1);
    gl_fbo* f = arg_fbo(env, 1);
    g->fbo = f;   /* NULL binds the default framebuffer */
    gl_pin(&g->fbo_var, f ? fv : NULL);
    return NULL;
}
static var_t* gl_framebufferTexture2D(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL || g->fbo == NULL) return NULL;
    if(js_arg_int(env, 0) != GL_FRAMEBUFFER) return NULL;
    if(js_arg_int(env, 1) != GL_COLOR_ATTACHMENT0) return NULL;
    if(js_arg_int(env, 2) != GL_TEXTURE_2D) return NULL;
    gl_fbo* f = g->fbo;
    var_t* tv = js_arg(env, 3);
    gl_texture* t = arg_texture(env, 3);
    f->color = t;
    gl_pin(&f->color_var, t ? tv : NULL);
    /* Re-size the store when the attachment changes dims (or detaches). */
    if(f->fb != NULL && (t == NULL || t->w != f->w || t->h != f->h)) {
        free(f->fb); free(f->depth);
        f->fb = NULL; f->depth = NULL; f->w = 0; f->h = 0;
    }
    return NULL;
}
static var_t* gl_deleteFramebuffer(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    gl_fbo* f = arg_fbo(env, 0);
    if(g == NULL || f == NULL) return NULL;
    f->deleted = 1;
    if(g->fbo == f) { g->fbo = NULL; gl_pin(&g->fbo_var, NULL); }
    return NULL;   /* real free happens when the JS object is collected */
}
static void rbo_free(gl_rbo* r) { free(r); }
static var_t* gl_createRenderbuffer(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    gl_rbo* r = (gl_rbo*)calloc(1, sizeof(gl_rbo));
    if(r == NULL) return var_new_null(vm);
    return var_new_obj(vm, g_rbo_proto, r, (free_func_t)rbo_free);
}
static var_t* gl_bindRenderbuffer(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL || js_arg_int(env, 0) != GL_RENDERBUFFER) return NULL;
    var_t* rv = js_arg(env, 1);
    gl_rbo* r = arg_rbo(env, 1);
    g->rbo = r;
    gl_pin(&g->rbo_var, r ? rv : NULL);
    return NULL;
}
static var_t* gl_renderbufferStorage(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL || g->rbo == NULL) return NULL;
    if(js_arg_int(env, 0) != GL_RENDERBUFFER) return NULL;
    g->rbo->fmt = js_arg_int(env, 1);
    g->rbo->w = js_arg_int(env, 2);
    g->rbo->h = js_arg_int(env, 3);
    return NULL;
}
static var_t* gl_framebufferRenderbuffer(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL || g->fbo == NULL) return NULL;
    if(js_arg_int(env, 0) != GL_FRAMEBUFFER) return NULL;
    int attachment = js_arg_int(env, 1);
    gl_rbo* r = arg_rbo(env, 3);
    if(attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT)
        g->fbo->has_depth = (r != NULL) ? 1 : 0;
    return NULL;
}
static var_t* gl_deleteRenderbuffer(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    gl_rbo* r = arg_rbo(env, 0);
    if(g == NULL || r == NULL) return NULL;
    r->deleted = 1;
    if(g->rbo == r) { g->rbo = NULL; gl_pin(&g->rbo_var, NULL); }
    return NULL;
}
static var_t* gl_checkFramebufferStatus(vm_t* vm, var_t* env, void* data) {
    (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL || js_arg_int(env, 0) != GL_FRAMEBUFFER) return var_new_int(vm, 0);
    if(g->fbo == NULL) return var_new_int(vm, GL_FRAMEBUFFER_COMPLETE);
    gl_texture* t = g->fbo->color;
    if(t != NULL && t->rgba != NULL && !t->deleted)
        return var_new_int(vm, GL_FRAMEBUFFER_COMPLETE);
    return var_new_int(vm, GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);
}

/* Bytes per pixel of an upload format (0 = unsupported). */
static int format_bpp(int fmt) {
    switch(fmt) {
        case GL_RGBA: return 4;
        case GL_RGB: return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_LUMINANCE: case GL_ALPHA: return 1;
        default: return 0;
    }
}
/* Expand one source pixel (format bpp) into RGBA8, honouring premultiply. */
static void expand_pixel(const uint8_t* src, int fmt, int premult, uint8_t* dst) {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    switch(fmt) {
        case GL_RGBA: r = src[0]; g = src[1]; b = src[2]; a = src[3]; break;
        case GL_RGB:  r = src[0]; g = src[1]; b = src[2]; break;
        case GL_LUMINANCE: r = g = b = src[0]; break;
        case GL_ALPHA: a = src[0]; break;
        case GL_LUMINANCE_ALPHA: r = g = b = src[0]; a = src[1]; break;
        default: break;
    }
    if(premult && a != 255) {
        r = (uint8_t)((r * a + 127) / 255);
        g = (uint8_t)((g * a + 127) / 255);
        b = (uint8_t)((b * a + 127) / 255);
    }
    dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
}
/* Read element `i` of a typed array / JS array as a byte value. */
static uint8_t arr_byte(vm_t* vm, var_t* arr, int i) {
    var_t* e;
    if(var_is_typedarray(arr)) {
        e = var_typedarray_get_at(vm, arr, i);
        int v = (int)js_num(e);
        if(e) var_unref(e);
        return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    node_t* nd = var_array_get(arr, i);
    e = (nd != NULL) ? nd->var : NULL;
    int v = (int)js_num(e);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}
/* Allocate / resize a texture's RGBA8 store (zero-filled). */
static int tex_alloc(gl_texture* t, int w, int h) {
    if(w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    uint8_t* px = (uint8_t*)calloc((size_t)w * (size_t)h, 4);
    if(px == NULL) return 0;
    free(t->rgba);
    t->rgba = px; t->w = w; t->h = h;
    return 1;
}
/* Upload a byte array (typed array / JS array / NULL) into the texture at
 * (xoff,yoff). Rows are padded to UNPACK_ALIGNMENT. flip_y reverses row order
 * within the uploaded rect (WebGL unpack semantics). */
static void tex_upload_bytes(vm_t* vm, gl_ctx* g, gl_texture* t, int xoff, int yoff,
                             int w, int h, int fmt, var_t* pixels) {
    int bpp = format_bpp(fmt);
    if(bpp == 0 || t->rgba == NULL) return;
    int align = g->unpack_alignment ? g->unpack_alignment : 4;
    int rowbytes = w * bpp;
    int stride = (rowbytes + align - 1) / align * align;
    int total = stride * h;
    uint8_t* tmp = NULL;
    if(pixels != NULL && (var_is_typedarray(pixels) || pixels->is_array)) {
        int n = var_is_typedarray(pixels) ? var_get_int(var_find_own_member_var(pixels, "length"))
                                          : (int)var_array_size(pixels);
        if(n > total) n = total;
        tmp = (uint8_t*)malloc((size_t)(total > 0 ? total : 1));
        if(tmp == NULL) return;
        memset(tmp, 0, (size_t)total);
        for(int i = 0; i < n; ++i) tmp[i] = arr_byte(vm, pixels, i);
    }
    for(int row = 0; row < h; ++row) {
        int dstrow = g->unpack_flip_y ? (yoff + h - 1 - row) : (yoff + row);
        if(dstrow < 0 || dstrow >= t->h) continue;
        const uint8_t* src = tmp ? tmp + (size_t)row * stride : NULL;
        for(int x = 0; x < w; ++x) {
            int dstx = xoff + x;
            if(dstx < 0 || dstx >= t->w) continue;
            uint8_t* dst = t->rgba + ((size_t)dstrow * t->w + dstx) * 4;
            if(src) expand_pixel(src + (size_t)x * bpp, fmt, g->unpack_premult, dst);
            else memset(dst, 0, 4);
        }
    }
    free(tmp);
}
/* Copy an embedder bitmap into the texture store. `bmp` is either a raw
 * eweb_surface_t* (is_canvas=0: bitmap_data bulk access) or an EWebCanvas
 * registry handle (is_canvas=1: per-pixel get_pixel - the bitmap_* callbacks
 * would mis-cast the C++ object as a surface). Same flip_y row semantics as
 * the byte path; premultiply honoured. */
static void tex_upload_bitmap(gl_ctx* g, gl_texture* t, int xoff, int yoff,
                              void* bmp, int sw, int sh, int is_canvas) {
    if(t->rgba == NULL || bmp == NULL || sw <= 0 || sh <= 0) return;
    int bw = 0, bh = 0;
    uint32_t* data = (!is_canvas && g->cb.bitmap_data != NULL)
        ? g->cb.bitmap_data(g->embed_ctx, bmp, &bw, &bh) : NULL;
    for(int row = 0; row < sh; ++row) {
        int dstrow = g->unpack_flip_y ? (yoff + sh - 1 - row) : (yoff + row);
        if(dstrow < 0 || dstrow >= t->h) continue;
        for(int x = 0; x < sw; ++x) {
            int dstx = xoff + x;
            if(dstx < 0 || dstx >= t->w) continue;
            uint32_t c = 0;
            if(data != NULL && row < bh && x < bw) c = data[(size_t)row * bw + x];
            else if(g->cb.get_pixel != NULL) c = g->cb.get_pixel(g->embed_ctx, bmp, x, row);
            uint8_t a = (uint8_t)(c >> 24), r = (uint8_t)(c >> 16),
                    gg = (uint8_t)(c >> 8), b = (uint8_t)c;
            if(g->unpack_premult && a != 255) {
                r = (uint8_t)((r * a + 127) / 255);
                gg = (uint8_t)((gg * a + 127) / 255);
                b = (uint8_t)((b * a + 127) / 255);
            }
            uint8_t* dst = t->rgba + ((size_t)dstrow * t->w + dstx) * 4;
            dst[0] = r; dst[1] = gg; dst[2] = b; dst[3] = a;
        }
    }
}
/* Class-name tag of a bridge instance (see js_canvas.c tag_class). */
static const char* obj_class_name(var_t* v) {
    if(v == NULL) return "";
    /* Walk the prototype chain: upgraded custom elements and subclass
     * wrappers carry their own prototype above the tagged bridge class. */
    var_t* proto = var_get_prototype(v);
    for(int hop = 0; proto != NULL && hop < 16; ++hop) {
        const char* s = var_get_str(var_find_own_member_var(proto, "__class_name"));
        if(getenv("EWEB_TEXDBG") != NULL)
            fprintf(stderr, "[texdbg] classwalk hop=%d proto=%p name='%s'\n",
                    hop, (void*)proto, s ? s : "(none)");
        /* var_get_str(NULL) returns "" - only a non-empty tag is a hit. */
        if(s != NULL && s[0] != 0) return s;
        proto = var_get_prototype(proto);
    }
    return "";
}
/* Resolve a texImage2D source var to an embedder bitmap + dims. Own canvas
 * resolves through the context handle; <img> through bitmap_from_element
 * (the same decoded-image cache litehtml uses); a 2D context through its
 * .canvas. *is_canvas reports the handle kind (registry EWebCanvas vs raw
 * surface) so the upload path picks the right pixel accessor. Returns 1 on
 * success. */
static int tex_resolve_source(gl_ctx* g, var_t* src, void** bmp, int* w, int* h, int* is_canvas) {
    *bmp = NULL; *w = 0; *h = 0; *is_canvas = 0;
    if(g == NULL || src == NULL) return 0;
    if(g->canvas_el != NULL && src == g->canvas_el) {
        *bmp = g->canvas;
        *is_canvas = 1;
        if(g->cb.canvas_dims != NULL) g->cb.canvas_dims(g->embed_ctx, g->canvas, w, h);
        else { *w = g->w; *h = g->h; }
        return (*bmp != NULL && *w > 0 && *h > 0);
    }
    const char* cname = obj_class_name(src);
    if(!strcmp(cname, "CanvasRenderingContext2D")) {
        return tex_resolve_source(g, var_find_own_member_var(src, "canvas"), bmp, w, h, is_canvas);
    }
    if(!strcmp(cname, "Element") && src->value != NULL) {
        /* <img>: the decoded bitmap from the container's image cache. */
        void* b = (g->cb.bitmap_from_element != NULL)
            ? g->cb.bitmap_from_element(g->embed_ctx, src->value) : NULL;
        if(b != NULL) {
            if(g->cb.bitmap_dims != NULL) g->cb.bitmap_dims(g->embed_ctx, b, w, h);
            if(*w <= 0 || *h <= 0) return 0;
            *bmp = b;
            return 1;
        }
        /* <canvas>: the element handle stays a litehtml element*; the backing
         * store lives in the embedder registry under the SAME key
         * js_el_getContext uses (id attr, else "@cv%p" of the handle).
         * Discriminate by the absent src attribute (an <img> always has one). */
        if(g->cb.canvas_create != NULL && g->cb.el_get_attr != NULL) {
            char* srcattr = g->cb.el_get_attr(g->embed_ctx, src->value, "src");
            int is_cv_el = (srcattr == NULL || srcattr[0] == 0);
            if(srcattr != NULL) mario_free(srcattr);
            if(is_cv_el) {
                char* id = g->cb.el_get_attr(g->embed_ctx, src->value, "id");
                char idbuf[48];
                const char* key = id;
                if(key == NULL || key[0] == 0) {
                    snprintf(idbuf, sizeof(idbuf), "@cv%p", src->value);
                    key = idbuf;
                }
                int cw = 300, ch = 150;
                char* aw = g->cb.el_get_attr(g->embed_ctx, src->value, "width");
                char* ah = g->cb.el_get_attr(g->embed_ctx, src->value, "height");
                if(aw != NULL) { int v = atoi(aw); if(v > 0) cw = v; mario_free(aw); }
                if(ah != NULL) { int v = atoi(ah); if(v > 0) ch = v; mario_free(ah); }
                void* b = g->cb.canvas_create(g->embed_ctx, key, cw, ch);
                if(id != NULL) mario_free(id);
                if(b != NULL && g->cb.canvas_dims != NULL) {
                    g->cb.canvas_dims(g->embed_ctx, b, w, h);
                    if(*w > 0 && *h > 0) { *bmp = b; *is_canvas = 1; return 1; }
                }
            }
        }
    }
    return 0;
}
/* ImageData source: read .width/.height/.data straight off the instance. */
static int tex_upload_imagedata(vm_t* vm, gl_ctx* g, gl_texture* t, int xoff, int yoff, var_t* src) {
    int w = var_get_int(var_find_own_member_var(src, "width"));
    int h = var_get_int(var_find_own_member_var(src, "height"));
    var_t* px = var_find_own_member_var(src, "data");
    if(w <= 0 || h <= 0 || px == NULL) return 0;
    if(xoff == 0 && yoff == 0 && !tex_alloc(t, w, h)) return 0;
    tex_upload_bytes(vm, g, t, xoff, yoff, w, h, GL_RGBA, px);
    return 1;
}
static var_t* gl_texImage2D(vm_t* vm, var_t* env, void* data) {
    (void)data;
    gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    if(js_arg_int(env, 0) != GL_TEXTURE_2D) return NULL;
    gl_texture* t = g->tex_unit[g->active_unit];
    if(t == NULL || js_arg_int(env, 1) != 0) return NULL;   /* base level only */
    int argc = js_arg_count(env);
    if(argc >= 9) {
        /* (target, level, internalformat, w, h, border, format, type, pixels) */
        int w = js_arg_int(env, 3), h = js_arg_int(env, 4);
        int fmt = js_arg_int(env, 6), type = js_arg_int(env, 7);
        if(type != GL_UNSIGNED_BYTE || format_bpp(fmt) == 0) return NULL;
        if(!tex_alloc(t, w, h)) return NULL;
        tex_upload_bytes(vm, g, t, 0, 0, w, h, fmt, js_arg(env, 8));
        return NULL;
    }
    if(argc >= 6) {
        /* (target, level, internalformat, format, type, source) */
        int type = js_arg_int(env, 4);
        if(type != GL_UNSIGNED_BYTE) return NULL;
        var_t* src = js_arg(env, 5);
        if(getenv("EWEB_TEXDBG") != NULL)
            fprintf(stderr, "[texdbg] texImage2D6 src=%p class='%s'\n",
                    src ? src->value : NULL, obj_class_name(src));
        if(!strcmp(obj_class_name(src), "ImageData")) {
            tex_upload_imagedata(vm, g, t, 0, 0, src);
            return NULL;
        }
        void* bmp = NULL; int w = 0, h = 0, iscv = 0;
        int ok = tex_resolve_source(g, src, &bmp, &w, &h, &iscv);
        if(getenv("EWEB_TEXDBG") != NULL)
            fprintf(stderr, "[texdbg] resolve ok=%d bmp=%p w=%d h=%d canvas=%d\n", ok, bmp, w, h, iscv);
        if(ok && tex_alloc(t, w, h))
            tex_upload_bitmap(g, t, 0, 0, bmp, w, h, iscv);
    }
    return NULL;
}
static var_t* gl_texSubImage2D(vm_t* vm, var_t* env, void* data) {
    (void)data;
    gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    if(js_arg_int(env, 0) != GL_TEXTURE_2D) return NULL;
    gl_texture* t = g->tex_unit[g->active_unit];
    if(t == NULL || t->rgba == NULL || js_arg_int(env, 1) != 0) return NULL;
    int xoff = js_arg_int(env, 2), yoff = js_arg_int(env, 3);
    int argc = js_arg_count(env);
    if(argc >= 9) {
        int w = js_arg_int(env, 4), h = js_arg_int(env, 5);
        int fmt = js_arg_int(env, 6), type = js_arg_int(env, 7);
        if(type != GL_UNSIGNED_BYTE || format_bpp(fmt) == 0) return NULL;
        tex_upload_bytes(vm, g, t, xoff, yoff, w, h, fmt, js_arg(env, 8));
        return NULL;
    }
    if(argc >= 7) {
        int type = js_arg_int(env, 5);
        if(type != GL_UNSIGNED_BYTE) return NULL;
        var_t* src = js_arg(env, 6);
        if(!strcmp(obj_class_name(src), "ImageData")) {
            tex_upload_imagedata(vm, g, t, xoff, yoff, src);
            return NULL;
        }
        void* bmp = NULL; int w = 0, h = 0, iscv = 0;
        if(tex_resolve_source(g, src, &bmp, &w, &h, &iscv))
            tex_upload_bitmap(g, t, xoff, yoff, bmp, w, h, iscv);
    }
    return NULL;
}
/* readPixels: copy the real framebuffer into a Uint8Array (RGBA/UNSIGNED_BYTE
 * is the WebGL-guaranteed combination). GL origin is bottom-left; our fb row 0
 * is top, so rows are flipped on the way out. Out-of-rect pixels read 0. */
static var_t* gl_readPixels(vm_t* vm, var_t* env, void* data) {
    (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int x = js_arg_int(env, 0), y = js_arg_int(env, 1);
    int w = js_arg_int(env, 2), h = js_arg_int(env, 3);
    int fmt = js_arg_int(env, 4), type = js_arg_int(env, 5);
    var_t* px = js_arg(env, 6);
    if(px == NULL || !var_is_typedarray(px)) return NULL;
    if(fmt != GL_RGBA || type != GL_UNSIGNED_BYTE) return NULL;
    if(w <= 0 || h <= 0) return NULL;
    int n = var_get_int(var_find_own_member_var(px, "length"));
    int tw = 0, th = 0;
    uint32_t* fb = gl_target(g, &tw, &th, NULL);
    for(int row = 0; row < h; ++row) {
        for(int col = 0; col < w; ++col) {
            int i = (row * w + col) * 4;
            if(i + 3 >= n) return NULL;
            int fx = x + col, fy = th - 1 - (y + row);
            uint32_t c = (fx >= 0 && fx < tw && fy >= 0 && fy < th)
                ? fb[fy * tw + fx] : 0;
            var_typedarray_set_at(vm, px, (int64_t)i + 0, var_new_int(vm, (c >> 16) & 0xff));
            var_typedarray_set_at(vm, px, (int64_t)i + 1, var_new_int(vm, (c >> 8) & 0xff));
            var_typedarray_set_at(vm, px, (int64_t)i + 2, var_new_int(vm, c & 0xff));
            var_typedarray_set_at(vm, px, (int64_t)i + 3, var_new_int(vm, (c >> 24) & 0xff));
        }
    }
    return NULL;
}
/* ------------------------------------------------------------------ */
/* State natives                                                       */
/* ------------------------------------------------------------------ */
static var_t* gl_clearColor(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    g->clear_r = (float)js_arg_num(env, 0); g->clear_g = (float)js_arg_num(env, 1);
    g->clear_b = (float)js_arg_num(env, 2); g->clear_a = (float)js_arg_num(env, 3);
    g->clear_color = pack_rgba(g->clear_r, g->clear_g, g->clear_b, g->clear_a);
    return NULL;
}
static var_t* gl_clear(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int tw = 0, th = 0; float* depth = NULL;
    uint32_t* fb = gl_target(g, &tw, &th, &depth);
    for(int i = 0; i < tw * th; ++i) {
        fb[i] = g->clear_color;
        if(depth) depth[i] = 1.0f;
    }
    if(g->fbo == NULL) gl_dirty_full(g);
    gl_flush(g);
    return NULL;
}
static var_t* gl_viewport(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    g->vp_x = js_arg_int(env, 0); g->vp_y = js_arg_int(env, 1);
    g->vp_w = js_arg_int(env, 2); g->vp_h = js_arg_int(env, 3);
    return NULL;
}
static var_t* gl_enable(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int cap = js_arg_int(env, 0);
    if(cap == GL_BLEND) g->blend_enabled = 1;
    else if(cap == GL_DEPTH_TEST) g->depth_test_enabled = 1;
    else if(cap == GL_CULL_FACE) g->cull_enabled = 1;
    else if(cap == GL_SCISSOR_TEST) g->scissor_enabled = 1;
    return NULL;
}
static var_t* gl_disable(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return NULL;
    int cap = js_arg_int(env, 0);
    if(cap == GL_BLEND) g->blend_enabled = 0;
    else if(cap == GL_DEPTH_TEST) g->depth_test_enabled = 0;
    else if(cap == GL_CULL_FACE) g->cull_enabled = 0;
    else if(cap == GL_SCISSOR_TEST) g->scissor_enabled = 0;
    return NULL;
}
static var_t* gl_isEnabled(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL) return var_new_bool(vm, false);
    int cap = js_arg_int(env, 0);
    int on = (cap == GL_BLEND) ? g->blend_enabled :
             (cap == GL_DEPTH_TEST) ? g->depth_test_enabled :
             (cap == GL_CULL_FACE) ? g->cull_enabled :
             (cap == GL_SCISSOR_TEST) ? g->scissor_enabled : 0;
    return var_new_bool(vm, on != 0);
}
static var_t* gl_blendFunc(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g) { g->blend_src = js_arg_int(env, 0); g->blend_dst = js_arg_int(env, 1); }
    return NULL;
}
static var_t* gl_cullFace(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g) g->cull_mode = js_arg_int(env, 0);
    return NULL;
}
static var_t* gl_frontFace(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g) g->front_face = js_arg_int(env, 0);
    return NULL;
}
static var_t* gl_getError(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data; return var_new_int(vm, GL_NO_ERROR);
}
static var_t* gl_noop(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)env; (void)data; return NULL;   /* accepted-but-inert calls */
}

/* ------------------------------------------------------------------ */
/* Query natives (HONEST software identity - never a fabricated GPU)   */
/* ------------------------------------------------------------------ */
static var_t* gl_getParameter(vm_t* vm, var_t* env, void* data) {
    (void)data; gl_ctx* g = this_ctx(env);
    int pname = js_arg_int(env, 0);
    switch(pname) {
        case GL_VENDOR:   return var_new_str(vm, "WebKit");
        case GL_RENDERER: return var_new_str(vm, "WebKit WebGL");
        case GL_VERSION:  return var_new_str(vm, "WebGL 1.0");
        case GL_SHADING_LANGUAGE_VERSION: return var_new_str(vm, "WebGL GLSL ES 1.0");
        /* Honest unmasked identity: a real software rasterizer, NOT a GPU lie. */
        case GL_UNMASKED_VENDOR_WEBGL:   return var_new_str(vm, "ewebview");
        case GL_UNMASKED_RENDERER_WEBGL: return var_new_str(vm, "ewebview software rasterizer");
        case GL_MAX_VERTEX_ATTRIBS:      return var_new_int(vm, GL_MAX_ATTRIBS);
        case GL_MAX_VERTEX_UNIFORM_VECTORS:  return var_new_int(vm, 256);
        case GL_MAX_FRAGMENT_UNIFORM_VECTORS: return var_new_int(vm, 256);
        case GL_MAX_VARYING_VECTORS:     return var_new_int(vm, 16);
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: return var_new_int(vm, 16);
        case GL_MAX_TEXTURE_IMAGE_UNITS: return var_new_int(vm, 16);
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: return var_new_int(vm, 32);
        case GL_MAX_TEXTURE_SIZE:        return var_new_int(vm, 4096);
        case GL_MAX_RENDERBUFFER_SIZE:   return var_new_int(vm, 4096);
        case GL_RED_BITS:   return var_new_int(vm, 8);
        case GL_GREEN_BITS: return var_new_int(vm, 8);
        case GL_BLUE_BITS:  return var_new_int(vm, 8);
        case GL_ALPHA_BITS: return var_new_int(vm, 8);
        case GL_DEPTH_BITS: return var_new_int(vm, 24);
        case GL_STENCIL_BITS: return var_new_int(vm, 8);
        case GL_UNPACK_FLIP_Y_WEBGL: return var_new_bool(vm, g && g->unpack_flip_y);
        case GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL: return var_new_bool(vm, g && g->unpack_premult);
        case GL_UNPACK_COLORSPACE_CONVERSION_WEBGL: return var_new_int(vm, 0x9244);
        case GL_MAX_VIEWPORT_DIMS:
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_ALIASED_POINT_SIZE_RANGE: {
            var_t* a = var_new_array(vm);
            if(pname == GL_MAX_VIEWPORT_DIMS) { var_array_add(a, var_new_int(vm, 4096)); var_array_add(a, var_new_int(vm, 4096)); }
            else if(pname == GL_ALIASED_LINE_WIDTH_RANGE) { var_array_add(a, var_new_float(vm, 1.0f)); var_array_add(a, var_new_float(vm, 1.0f)); }
            else { var_array_add(a, var_new_float(vm, 1.0f)); var_array_add(a, var_new_float(vm, 255.0f)); }
            return a;
        }
        default: break;
    }
    /* A few state values that depend on the live context. */
    if(pname == GL_TEXTURE_BINDING_2D) {
        if(g == NULL || g->tex_unit_var[g->active_unit] == NULL) return var_new_null(vm);
        return var_ref(g->tex_unit_var[g->active_unit]);
    }
    if(pname == 0x8894 /*ARRAY_BUFFER_BINDING*/ && g) {
        if(g->array_buffer_var == NULL) return var_new_null(vm);
        return var_ref(g->array_buffer_var);
    }
    if(pname == 0x8895 /*ELEMENT_ARRAY_BUFFER_BINDING*/ && g) {
        if(g->element_buffer_var == NULL) return var_new_null(vm);
        return var_ref(g->element_buffer_var);
    }
    if(pname == GL_FRAMEBUFFER_BINDING) {
        if(g == NULL || g->fbo_var == NULL) return var_new_null(vm);
        return var_ref(g->fbo_var);
    }
    if(pname == GL_RENDERBUFFER_BINDING) {
        if(g == NULL || g->rbo_var == NULL) return var_new_null(vm);
        return var_ref(g->rbo_var);
    }
    if(pname == 0x0BA2 /*VIEWPORT*/ && g) {
        var_t* a = var_new_array(vm);
        var_array_add(a, var_new_int(vm, g->vp_x)); var_array_add(a, var_new_int(vm, g->vp_y));
        var_array_add(a, var_new_int(vm, g->vp_w)); var_array_add(a, var_new_int(vm, g->vp_h));
        return a;
    }
    if(pname == 0x1F01 /*RENDERER dup*/) return var_new_str(vm, "WebKit WebGL");
    return var_new_null(vm);
}
static var_t* gl_getSupportedExtensions(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    /* Only extensions we genuinely implement are advertised - advertising an
     * unimplemented one would let a page rely on behaviour we cannot honour. */
    var_t* a = var_new_array(vm);
    var_array_add(a, var_new_str(vm, "WEBGL_debug_renderer_info"));
    return a;
}
static var_t* gl_getExtension(vm_t* vm, var_t* env, void* data) {
    (void)data;
    mstr_t* m = mstr_new("");
    const char* name = js_cstr(js_arg(env, 0), m);
    var_t* r;
    if(name && !strcmp(name, "WEBGL_debug_renderer_info")) {
        r = plain_obj(vm);
        var_add(r, "UNMASKED_VENDOR_WEBGL",   var_new_int(vm, GL_UNMASKED_VENDOR_WEBGL));
        var_add(r, "UNMASKED_RENDERER_WEBGL", var_new_int(vm, GL_UNMASKED_RENDERER_WEBGL));
    } else {
        r = var_new_null(vm);   /* unsupported extension -> null (spec) */
    }
    mstr_free(m);
    return r;
}
static var_t* gl_getShaderPrecisionFormat(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    int ptype = js_arg_int(env, 1);
    int precision, rmin, rmax;
    switch(ptype) {
        case GL_LOW_FLOAT:    precision = 8;  rmin = 127; rmax = 127; break;
        case GL_MEDIUM_FLOAT: precision = 23; rmin = 127; rmax = 127; break;
        case GL_HIGH_FLOAT:   precision = 23; rmin = 127; rmax = 127; break;
        case GL_LOW_INT:      precision = 0;  rmin = 31;  rmax = 30;  break;
        case GL_MEDIUM_INT:   precision = 0;  rmin = 31;  rmax = 30;  break;
        case GL_HIGH_INT:     precision = 0;  rmin = 31;  rmax = 30;  break;
        default:              precision = 23; rmin = 127; rmax = 127; break;
    }
    var_t* o = plain_obj(vm);
    var_add(o, "rangeMin",  var_new_int(vm, rmin));
    var_add(o, "rangeMax",  var_new_int(vm, rmax));
    var_add(o, "precision", var_new_int(vm, precision));
    return o;
}
static var_t* gl_getContextAttributes(vm_t* vm, var_t* env, void* data) {
    (void)env; (void)data;
    var_t* o = plain_obj(vm);
    var_add(o, "alpha", var_new_bool(vm, true));
    var_add(o, "depth", var_new_bool(vm, true));
    var_add(o, "stencil", var_new_bool(vm, false));
    var_add(o, "antialias", var_new_bool(vm, false));
    var_add(o, "premultipliedAlpha", var_new_bool(vm, true));
    var_add(o, "preserveDrawingBuffer", var_new_bool(vm, false));
    return o;
}
static var_t* gl_get_canvas(vm_t* vm, var_t* env, void* data) {
    (void)vm; (void)data; gl_ctx* g = this_ctx(env);
    if(g == NULL || g->canvas_el == NULL) return var_new_null(vm);
    return var_ref(g->canvas_el);
}
/* drawingBufferWidth/Height: the actual framebuffer dimensions (spec: may be
 * smaller than canvas.width/height on devicePixelRatio clamping; ours is 1:1).
 * AMap 2.0's capability probe rejects any context where these are undefined
 * (`n.drawingBufferWidth!==k.width` -> false), so without them every WebGL
 * renderer selection silently falls back even though the context is real. */
static var_t* gl_get_drawingBufferWidth(vm_t* vm, var_t* env, void* data) {
    (void)data; gl_ctx* g = this_ctx(env);
    return var_new_int(vm, (g != NULL) ? g->w : 0);
}
static var_t* gl_get_drawingBufferHeight(vm_t* vm, var_t* env, void* data) {
    (void)data; gl_ctx* g = this_ctx(env);
    return var_new_int(vm, (g != NULL) ? g->h : 0);
}
/* ------------------------------------------------------------------ */
/* Context teardown + creation                                         */
/* ------------------------------------------------------------------ */
static void gl_ctx_free(void* p) {
    gl_ctx* g = (gl_ctx*)p;
    if(g == NULL) return;
    free(g->fb); free(g->depth); free(g);
}
/* on_destroy runs while the var is still valid: drop the ref'd canvas Element
 * (the gl_ctx holds a var_t* reference, so it must be released here, not in the
 * plain free_func - see the C-struct-holding-var guardrail). */
static void gl_ctx_on_destroy(void* vp) {
    var_t* v = (var_t*)vp;
    if(v == NULL) return;
    gl_ctx* g = (gl_ctx*)v->value;
    if(g == NULL) return;
    if(g->canvas_el != NULL) { var_unref(g->canvas_el); g->canvas_el = NULL; }
    gl_pin(&g->program_var, NULL);
    gl_pin(&g->array_buffer_var, NULL);
    gl_pin(&g->element_buffer_var, NULL);
    for(int i = 0; i < GL_MAX_ATTRIBS; ++i) gl_pin(&g->attrib[i].buffer_var, NULL);
    for(int i = 0; i < GL_MAX_TEXTURE_UNITS; ++i) gl_pin(&g->tex_unit_var[i], NULL);
    gl_pin(&g->fbo_var, NULL);
    gl_pin(&g->rbo_var, NULL);
}

var_t* js_webgl_create_context(vm_t* vm, int w, int h,
                               void* embed_ctx, void* canvas,
                               webgl_set_pixel_fn setpx, var_t* canvas_el,
                               const struct js_canvas_callbacks* cb) {
    /* The pins must belong to THIS vm (see g_reg_vm). */
    if(vm == NULL || g_reg_vm != vm || g_ctx_proto == NULL) return var_new_null(vm);
    if(w <= 0) w = 300;
    if(h <= 0) h = 150;
    gl_ctx* g = (gl_ctx*)calloc(1, sizeof(gl_ctx));
    if(g == NULL) return var_new_null(vm);
    g->w = w; g->h = h;
    g->fb = (uint32_t*)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    g->depth = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if(g->fb == NULL) { free(g->depth); free(g); return var_new_null(vm); }
    if(g->depth) for(int i = 0; i < w * h; ++i) g->depth[i] = 1.0f;
    g->clear_color = 0;
    g->vp_x = 0; g->vp_y = 0; g->vp_w = w; g->vp_h = h;
    g->front_face = GL_CCW; g->cull_mode = GL_BACK;
    g->blend_src = GL_ONE; g->blend_dst = GL_ZERO;
    g->embed_ctx = embed_ctx; g->canvas = canvas; g->setpx = setpx;
    if(cb != NULL) g->cb = *cb;
    g->unpack_alignment = 4;   /* GL default */
    g->dx0 = 1; g->dy0 = 1; g->dx1 = 0; g->dy1 = 0;   /* empty dirty rect */
    if(canvas_el != NULL) g->canvas_el = var_ref(canvas_el);
    var_t* obj = var_new_obj(vm, g_ctx_proto, g, gl_ctx_free);
    if(obj == NULL) { if(g->canvas_el) var_unref(g->canvas_el); gl_ctx_free(g); return var_new_null(vm); }
    obj->on_destroy = gl_ctx_on_destroy;
    return obj;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */
static void add_const(vm_t* vm, var_t* proto, const char* name, int val) {
    var_add(proto, name, var_new_int(vm, val));
}
static void tag_tostring(vm_t* vm, var_t* proto, const char* name) {
    if(proto == NULL) return;
    node_t* n = var_add(proto, SYMKEY_TOSTRINGTAG, var_new_str(vm, name));
    if(n != NULL) { n->invisable = 1; n->be_unenumerable = 1; }
}
/* Opt-in call trace (EWEB_GLTRACE=1): every context method logs its name in
 * invocation order so a fingerprint-SDK abort can be pinned to the last call
 * before silence. Off by default and a single getenv-cached branch otherwise. */
typedef struct gl_regent { const char* decl; int namelen; native_func_t fn; } gl_regent;
static gl_regent g_regents[256];
static int       g_nregents = 0;
static int       gl_trace_on = -1;
static var_t* gl_tramp(vm_t* vm, var_t* env, void* data) {
    gl_regent* e = (gl_regent*)data;
    if(gl_trace_on < 0) gl_trace_on = (getenv("EWEB_GLTRACE") != NULL) ? 1 : 0;
    if(gl_trace_on && e != NULL)
        fprintf(stderr, "[gltrace] %.*s\n", e->namelen, e->decl);
    return e->fn(vm, env, NULL);
}
static void reg(vm_t* vm, var_t* cls, const char* decl, native_func_t fn) {
    if(g_nregents < 256) {
        int i = 0;
        while(decl[i] != 0 && decl[i] != '(') i++;
        g_regents[g_nregents].decl = decl;
        g_regents[g_nregents].namelen = i;
        g_regents[g_nregents].fn = fn;
        vm_reg_native(vm, cls, decl, gl_tramp, &g_regents[g_nregents]);
        g_nregents++;
        return;
    }
    vm_reg_native(vm, cls, decl, fn, NULL);
}

/* Drop the pins when their VM closes: vm_close() frees every var they point
 * at, and the next VM re-registers from scratch. */
static void gl_unpin(void) {
    g_ctx_cls = NULL;   g_ctx_proto = NULL;
    g_buf_cls = NULL;   g_buf_proto = NULL;
    g_shd_cls = NULL;   g_shd_proto = NULL;
    g_prg_cls = NULL;   g_prg_proto = NULL;
    g_uloc_cls = NULL;  g_uloc_proto = NULL;
    g_ainfo_cls = NULL; g_ainfo_proto = NULL;
    g_tex_cls = NULL;   g_tex_proto = NULL;
    g_fbo_cls = NULL;   g_fbo_proto = NULL;
    g_rbo_cls = NULL;   g_rbo_proto = NULL;
    g_nregents = 0;
    g_reg_vm = NULL;
}

static void gl_vm_close(void* data) {
    if(g_reg_vm == (vm_t*)data)
        gl_unpin();
}

bool js_webgl_register(vm_t* vm) {
    if(vm == NULL) return false;
    if(g_reg_vm == vm) return true;
    /* A different (or no) VM owns the pins: they are stale for this one. */
    gl_unpin();

    /* Pin every class var (kept alive for the vm lifetime) and build instances
     * from the pinned prototype directly - never by global-name lookup, which a
     * page could overwrite to hijack construction. */
    g_ctx_cls   = vm_new_class(vm, "WebGLRenderingContext");
    g_buf_cls   = vm_new_class(vm, "WebGLBuffer");
    g_shd_cls   = vm_new_class(vm, "WebGLShader");
    g_prg_cls   = vm_new_class(vm, "WebGLProgram");
    g_uloc_cls  = vm_new_class(vm, "WebGLUniformLocation");
    g_ainfo_cls = vm_new_class(vm, "WebGLActiveInfo");
    g_tex_cls   = vm_new_class(vm, "WebGLTexture");
    g_fbo_cls   = vm_new_class(vm, "WebGLFramebuffer");
    g_rbo_cls   = vm_new_class(vm, "WebGLRenderbuffer");
    if(g_ctx_cls == NULL) return false;
    g_ctx_cls = var_ref(g_ctx_cls);
    if(g_buf_cls)   g_buf_cls = var_ref(g_buf_cls);
    if(g_shd_cls)   g_shd_cls = var_ref(g_shd_cls);
    if(g_prg_cls)   g_prg_cls = var_ref(g_prg_cls);
    if(g_uloc_cls)  g_uloc_cls = var_ref(g_uloc_cls);
    if(g_ainfo_cls) g_ainfo_cls = var_ref(g_ainfo_cls);
    if(g_tex_cls)   g_tex_cls = var_ref(g_tex_cls);
    if(g_fbo_cls)   g_fbo_cls = var_ref(g_fbo_cls);
    if(g_rbo_cls)   g_rbo_cls = var_ref(g_rbo_cls);
    g_ctx_proto   = var_get_prototype(g_ctx_cls);
    g_buf_proto   = g_buf_cls   ? var_get_prototype(g_buf_cls)   : NULL;
    g_shd_proto   = g_shd_cls   ? var_get_prototype(g_shd_cls)   : NULL;
    g_prg_proto   = g_prg_cls   ? var_get_prototype(g_prg_cls)   : NULL;
    g_uloc_proto  = g_uloc_cls  ? var_get_prototype(g_uloc_cls)  : NULL;
    g_ainfo_proto = g_ainfo_cls ? var_get_prototype(g_ainfo_cls) : NULL;
    g_tex_proto   = g_tex_cls   ? var_get_prototype(g_tex_cls)   : NULL;
    g_fbo_proto   = g_fbo_cls   ? var_get_prototype(g_fbo_cls)   : NULL;
    g_rbo_proto   = g_rbo_cls   ? var_get_prototype(g_rbo_cls)   : NULL;
    tag_tostring(vm, g_ctx_proto,   "WebGLRenderingContext");
    tag_tostring(vm, g_buf_proto,   "WebGLBuffer");
    tag_tostring(vm, g_shd_proto,   "WebGLShader");
    tag_tostring(vm, g_prg_proto,   "WebGLProgram");
    tag_tostring(vm, g_uloc_proto,  "WebGLUniformLocation");
    tag_tostring(vm, g_ainfo_proto, "WebGLActiveInfo");
    tag_tostring(vm, g_tex_proto,   "WebGLTexture");
    tag_tostring(vm, g_fbo_proto,   "WebGLFramebuffer");
    tag_tostring(vm, g_rbo_proto,   "WebGLRenderbuffer");

    /* ---- methods ---- */
    reg(vm, g_ctx_cls, "createBuffer()",                              gl_createBuffer);
    reg(vm, g_ctx_cls, "bindBuffer(target, buffer)",                  gl_bindBuffer);
    reg(vm, g_ctx_cls, "bufferData(target, data, usage)",             gl_bufferData);
    reg(vm, g_ctx_cls, "deleteBuffer(buffer)",                        gl_deleteBuffer);
    reg(vm, g_ctx_cls, "createShader(type)",                          gl_createShader);
    reg(vm, g_ctx_cls, "shaderSource(shader, source)",                gl_shaderSource);
    reg(vm, g_ctx_cls, "compileShader(shader)",                       gl_compileShader);
    reg(vm, g_ctx_cls, "getShaderParameter(shader, pname)",           gl_getShaderParameter);
    reg(vm, g_ctx_cls, "getShaderInfoLog(shader)",                    gl_getShaderInfoLog);
    reg(vm, g_ctx_cls, "deleteShader(shader)",                        gl_deleteShader);
    reg(vm, g_ctx_cls, "createProgram()",                             gl_createProgram);
    reg(vm, g_ctx_cls, "attachShader(program, shader)",               gl_attachShader);
    reg(vm, g_ctx_cls, "linkProgram(program)",                        gl_linkProgram);
    reg(vm, g_ctx_cls, "useProgram(program)",                         gl_useProgram);
    reg(vm, g_ctx_cls, "getProgramParameter(program, pname)",         gl_getProgramParameter);
    reg(vm, g_ctx_cls, "getProgramInfoLog(program)",                  gl_getProgramInfoLog);
    reg(vm, g_ctx_cls, "deleteProgram(program)",                      gl_deleteProgram);
    reg(vm, g_ctx_cls, "getAttribLocation(program, name)",            gl_getAttribLocation);
    reg(vm, g_ctx_cls, "getUniformLocation(program, name)",           gl_getUniformLocation);
    reg(vm, g_ctx_cls, "uniform1f(location, x)",                      gl_uniform1f);
    reg(vm, g_ctx_cls, "uniform1i(location, x)",                      gl_uniform1i);
    reg(vm, g_ctx_cls, "uniform2f(location, x, y)",                   gl_uniform2f);
    reg(vm, g_ctx_cls, "uniform2i(location, x, y)",                   gl_uniform2i);
    reg(vm, g_ctx_cls, "uniform3f(location, x, y, z)",                gl_uniform3f);
    reg(vm, g_ctx_cls, "uniform4f(location, x, y, z, w)",             gl_uniform4f);
    reg(vm, g_ctx_cls, "uniform1fv(location, value)",                 gl_uniformNfv);
    reg(vm, g_ctx_cls, "uniform2fv(location, value)",                 gl_uniformNfv);
    reg(vm, g_ctx_cls, "uniform3fv(location, value)",                 gl_uniformNfv);
    reg(vm, g_ctx_cls, "uniform4fv(location, value)",                 gl_uniformNfv);
    reg(vm, g_ctx_cls, "uniform1iv(location, value)",                 gl_uniformNfv);
    reg(vm, g_ctx_cls, "uniformMatrix2fv(location, transpose, value)", gl_uniformMatrixNfv);
    reg(vm, g_ctx_cls, "uniformMatrix3fv(location, transpose, value)", gl_uniformMatrixNfv);
    reg(vm, g_ctx_cls, "uniformMatrix4fv(location, transpose, value)", gl_uniformMatrixNfv);
    reg(vm, g_ctx_cls, "enableVertexAttribArray(index)",              gl_enableVertexAttribArray);
    reg(vm, g_ctx_cls, "disableVertexAttribArray(index)",             gl_disableVertexAttribArray);
    reg(vm, g_ctx_cls, "vertexAttribPointer(index, size, type, normalized, stride, offset)", gl_vertexAttribPointer);
    reg(vm, g_ctx_cls, "drawArrays(mode, first, count)",              gl_drawArrays);
    reg(vm, g_ctx_cls, "drawElements(mode, count, type, offset)",     gl_drawElements);
    reg(vm, g_ctx_cls, "clearColor(r, g, b, a)",                      gl_clearColor);
    reg(vm, g_ctx_cls, "clear(mask)",                                 gl_clear);
    reg(vm, g_ctx_cls, "viewport(x, y, w, h)",                        gl_viewport);
    reg(vm, g_ctx_cls, "enable(cap)",                                 gl_enable);
    reg(vm, g_ctx_cls, "disable(cap)",                                gl_disable);
    reg(vm, g_ctx_cls, "isEnabled(cap)",                              gl_isEnabled);
    reg(vm, g_ctx_cls, "blendFunc(sfactor, dfactor)",                 gl_blendFunc);
    reg(vm, g_ctx_cls, "cullFace(mode)",                              gl_cullFace);
    reg(vm, g_ctx_cls, "frontFace(mode)",                             gl_frontFace);
    reg(vm, g_ctx_cls, "getError()",                                  gl_getError);
    reg(vm, g_ctx_cls, "getParameter(pname)",                         gl_getParameter);
    reg(vm, g_ctx_cls, "getSupportedExtensions()",                    gl_getSupportedExtensions);
    reg(vm, g_ctx_cls, "getExtension(name)",                          gl_getExtension);
    reg(vm, g_ctx_cls, "getShaderPrecisionFormat(shadertype, precisiontype)", gl_getShaderPrecisionFormat);
    reg(vm, g_ctx_cls, "getContextAttributes()",                      gl_getContextAttributes);
    /* Texture pipeline: real RGBA8 storage + sampling (see shade_sample). */
    reg(vm, g_ctx_cls, "createTexture()",                             gl_createTexture);
    reg(vm, g_ctx_cls, "activeTexture(texture)",                      gl_activeTexture);
    reg(vm, g_ctx_cls, "bindTexture(target, texture)",                gl_bindTexture);
    reg(vm, g_ctx_cls, "texParameteri(target, pname, param)",         gl_texParameteri);
    reg(vm, g_ctx_cls, "pixelStorei(pname, param)",                   gl_pixelStorei);
    reg(vm, g_ctx_cls, "texImage2D(target, level, ifmt, a, b, c, d, e, f)", gl_texImage2D);
    reg(vm, g_ctx_cls, "texSubImage2D(target, level, x, y, a, b, c, d, e)", gl_texSubImage2D);
    reg(vm, g_ctx_cls, "deleteTexture(texture)",                      gl_deleteTexture);
    reg(vm, g_ctx_cls, "readPixels(x, y, width, height, format, type, pixels)", gl_readPixels);
    /* Accepted-but-inert calls (framebuffer / stencil / misc state).
     * Registering them keeps `if (typeof gl.x === 'function')` capability probes
     * truthful for the drawing pipeline while making the unimplemented ones no-ops
     * rather than throwing "not a function". */
    reg(vm, g_ctx_cls, "depthFunc(func)",                             gl_noop);
    reg(vm, g_ctx_cls, "depthMask(flag)",                             gl_noop);
    reg(vm, g_ctx_cls, "clearDepth(v)",                               gl_noop);
    reg(vm, g_ctx_cls, "clearStencil(v)",                             gl_noop);
    reg(vm, g_ctx_cls, "colorMask(r, g, b, a)",                       gl_noop);
    reg(vm, g_ctx_cls, "scissor(x, y, w, h)",                         gl_noop);
    reg(vm, g_ctx_cls, "lineWidth(w)",                                gl_noop);
    reg(vm, g_ctx_cls, "blendFuncSeparate(a, b, c, d)",               gl_noop);
    reg(vm, g_ctx_cls, "blendEquation(mode)",                         gl_noop);
    reg(vm, g_ctx_cls, "stencilFunc(a, b, c)",                        gl_noop);
    reg(vm, g_ctx_cls, "stencilOp(a, b, c)",                          gl_noop);
    reg(vm, g_ctx_cls, "hint(target, mode)",                          gl_noop);
    reg(vm, g_ctx_cls, "polygonOffset(a, b)",                         gl_noop);
    reg(vm, g_ctx_cls, "sampleCoverage(v, invert)",                   gl_noop);
    /* generateMipmap: we keep a single mip level; mipmap filters sample it. */
    reg(vm, g_ctx_cls, "generateMipmap(target)",                      gl_noop);
    reg(vm, g_ctx_cls, "flush()",                                     gl_noop);
    reg(vm, g_ctx_cls, "finish()",                                    gl_noop);
    reg(vm, g_ctx_cls, "bindFramebuffer(target, fb)",                 gl_bindFramebuffer);
    reg(vm, g_ctx_cls, "bindRenderbuffer(target, rb)",                gl_bindRenderbuffer);
    reg(vm, g_ctx_cls, "deleteFramebuffer(f)",                        gl_deleteFramebuffer);
    reg(vm, g_ctx_cls, "deleteRenderbuffer(r)",                       gl_deleteRenderbuffer);
    /* FBO pipeline: real offscreen color store synced into the attached
     * texture on every flush (see gl_flush / gl_target). */
    reg(vm, g_ctx_cls, "createFramebuffer()",                         gl_createFramebuffer);
    reg(vm, g_ctx_cls, "framebufferTexture2D(target, attachment, textarget, texture, level)", gl_framebufferTexture2D);
    reg(vm, g_ctx_cls, "createRenderbuffer()",                        gl_createRenderbuffer);
    reg(vm, g_ctx_cls, "renderbufferStorage(target, internalformat, width, height)", gl_renderbufferStorage);
    reg(vm, g_ctx_cls, "framebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer)", gl_framebufferRenderbuffer);
    reg(vm, g_ctx_cls, "checkFramebufferStatus(target)",              gl_checkFramebufferStatus);
    js_acc_cls(vm, g_ctx_cls, "canvas", gl_get_canvas, NULL, NULL);
    js_acc_cls(vm, g_ctx_cls, "drawingBufferWidth", gl_get_drawingBufferWidth, NULL, NULL);
    js_acc_cls(vm, g_ctx_cls, "drawingBufferHeight", gl_get_drawingBufferHeight, NULL, NULL);

    /* ---- numeric constants on the prototype (visible on every instance) ---- */
    {
        var_t* p = g_ctx_proto;
        add_const(vm, p, "DEPTH_BUFFER_BIT", GL_DEPTH_BUFFER_BIT);
        add_const(vm, p, "STENCIL_BUFFER_BIT", GL_STENCIL_BUFFER_BIT);
        add_const(vm, p, "COLOR_BUFFER_BIT", GL_COLOR_BUFFER_BIT);
        add_const(vm, p, "POINTS", GL_POINTS);
        add_const(vm, p, "LINES", GL_LINES);
        add_const(vm, p, "LINE_LOOP", GL_LINE_LOOP);
        add_const(vm, p, "LINE_STRIP", GL_LINE_STRIP);
        add_const(vm, p, "TRIANGLES", GL_TRIANGLES);
        add_const(vm, p, "TRIANGLE_STRIP", GL_TRIANGLE_STRIP);
        add_const(vm, p, "TRIANGLE_FAN", GL_TRIANGLE_FAN);
        add_const(vm, p, "ZERO", GL_ZERO);
        add_const(vm, p, "ONE", GL_ONE);
        add_const(vm, p, "SRC_COLOR", GL_SRC_COLOR);
        add_const(vm, p, "ONE_MINUS_SRC_COLOR", GL_ONE_MINUS_SRC_COLOR);
        add_const(vm, p, "SRC_ALPHA", GL_SRC_ALPHA);
        add_const(vm, p, "ONE_MINUS_SRC_ALPHA", GL_ONE_MINUS_SRC_ALPHA);
        add_const(vm, p, "DST_ALPHA", GL_DST_ALPHA);
        add_const(vm, p, "ONE_MINUS_DST_ALPHA", GL_ONE_MINUS_DST_ALPHA);
        add_const(vm, p, "DST_COLOR", GL_DST_COLOR);
        add_const(vm, p, "ONE_MINUS_DST_COLOR", GL_ONE_MINUS_DST_COLOR);
        add_const(vm, p, "CULL_FACE", GL_CULL_FACE);
        add_const(vm, p, "DEPTH_TEST", GL_DEPTH_TEST);
        add_const(vm, p, "BLEND", GL_BLEND);
        add_const(vm, p, "SCISSOR_TEST", GL_SCISSOR_TEST);
        add_const(vm, p, "DITHER", GL_DITHER);
        add_const(vm, p, "FRONT", GL_FRONT);
        add_const(vm, p, "BACK", GL_BACK);
        add_const(vm, p, "FRONT_AND_BACK", 0x0408);
        add_const(vm, p, "CW", GL_CW);
        add_const(vm, p, "CCW", GL_CCW);
        add_const(vm, p, "NEVER", GL_NEVER);
        add_const(vm, p, "LESS", GL_LESS);
        add_const(vm, p, "LEQUAL", GL_LEQUAL);
        add_const(vm, p, "GREATER", GL_GREATER);
        add_const(vm, p, "GEQUAL", GL_GEQUAL);
        add_const(vm, p, "EQUAL", GL_EQUAL);
        add_const(vm, p, "NOTEQUAL", GL_NOTEQUAL);
        add_const(vm, p, "ALWAYS", GL_ALWAYS);
        add_const(vm, p, "BYTE", GL_BYTE);
        add_const(vm, p, "UNSIGNED_BYTE", GL_UNSIGNED_BYTE);
        add_const(vm, p, "SHORT", GL_SHORT);
        add_const(vm, p, "UNSIGNED_SHORT", GL_UNSIGNED_SHORT);
        add_const(vm, p, "INT", GL_INT);
        add_const(vm, p, "UNSIGNED_INT", GL_UNSIGNED_INT);
        add_const(vm, p, "FLOAT", GL_FLOAT);
        add_const(vm, p, "FIXED", GL_FIXED);
        add_const(vm, p, "ARRAY_BUFFER", GL_ARRAY_BUFFER);
        add_const(vm, p, "ELEMENT_ARRAY_BUFFER", GL_ELEMENT_ARRAY_BUFFER);
        add_const(vm, p, "STREAM_DRAW", GL_STREAM_DRAW);
        add_const(vm, p, "STATIC_DRAW", GL_STATIC_DRAW);
        add_const(vm, p, "DYNAMIC_DRAW", GL_DYNAMIC_DRAW);
        add_const(vm, p, "FRAGMENT_SHADER", GL_FRAGMENT_SHADER);
        add_const(vm, p, "VERTEX_SHADER", GL_VERTEX_SHADER);
        add_const(vm, p, "COMPILE_STATUS", GL_COMPILE_STATUS);
        add_const(vm, p, "LINK_STATUS", GL_LINK_STATUS);
        add_const(vm, p, "VALIDATE_STATUS", GL_VALIDATE_STATUS);
        add_const(vm, p, "DELETE_STATUS", GL_DELETE_STATUS);
        add_const(vm, p, "SHADER_TYPE", GL_SHADER_TYPE);
        add_const(vm, p, "INFO_LOG_LENGTH", GL_INFO_LOG_LENGTH);
        add_const(vm, p, "VENDOR", GL_VENDOR);
        add_const(vm, p, "RENDERER", GL_RENDERER);
        add_const(vm, p, "VERSION", GL_VERSION);
        add_const(vm, p, "SHADING_LANGUAGE_VERSION", GL_SHADING_LANGUAGE_VERSION);
        add_const(vm, p, "MAX_TEXTURE_SIZE", GL_MAX_TEXTURE_SIZE);
        add_const(vm, p, "MAX_RENDERBUFFER_SIZE", GL_MAX_RENDERBUFFER_SIZE);
        add_const(vm, p, "MAX_VERTEX_ATTRIBS", GL_MAX_VERTEX_ATTRIBS);
        add_const(vm, p, "MAX_VERTEX_UNIFORM_VECTORS", GL_MAX_VERTEX_UNIFORM_VECTORS);
        add_const(vm, p, "MAX_VARYING_VECTORS", GL_MAX_VARYING_VECTORS);
        add_const(vm, p, "MAX_VERTEX_TEXTURE_IMAGE_UNITS", GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS);
        add_const(vm, p, "MAX_COMBINED_TEXTURE_IMAGE_UNITS", GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
        add_const(vm, p, "MAX_TEXTURE_IMAGE_UNITS", GL_MAX_TEXTURE_IMAGE_UNITS);
        add_const(vm, p, "MAX_FRAGMENT_UNIFORM_VECTORS", GL_MAX_FRAGMENT_UNIFORM_VECTORS);
        add_const(vm, p, "MAX_VIEWPORT_DIMS", GL_MAX_VIEWPORT_DIMS);
        add_const(vm, p, "ALIASED_LINE_WIDTH_RANGE", GL_ALIASED_LINE_WIDTH_RANGE);
        add_const(vm, p, "ALIASED_POINT_SIZE_RANGE", GL_ALIASED_POINT_SIZE_RANGE);
        add_const(vm, p, "RED_BITS", GL_RED_BITS);
        add_const(vm, p, "GREEN_BITS", GL_GREEN_BITS);
        add_const(vm, p, "BLUE_BITS", GL_BLUE_BITS);
        add_const(vm, p, "ALPHA_BITS", GL_ALPHA_BITS);
        add_const(vm, p, "DEPTH_BITS", GL_DEPTH_BITS);
        add_const(vm, p, "STENCIL_BITS", GL_STENCIL_BITS);
        add_const(vm, p, "UNPACK_FLIP_Y_WEBGL", GL_UNPACK_FLIP_Y_WEBGL);
        add_const(vm, p, "UNPACK_PREMULTIPLY_ALPHA_WEBGL", GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL);
        add_const(vm, p, "UNPACK_COLORSPACE_CONVERSION_WEBGL", GL_UNPACK_COLORSPACE_CONVERSION_WEBGL);
        add_const(vm, p, "CONTEXT_LOST_WEBGL", GL_CONTEXT_LOST_WEBGL);
        add_const(vm, p, "UNMASKED_VENDOR_WEBGL", GL_UNMASKED_VENDOR_WEBGL);
        add_const(vm, p, "UNMASKED_RENDERER_WEBGL", GL_UNMASKED_RENDERER_WEBGL);
        add_const(vm, p, "NO_ERROR", GL_NO_ERROR);
        add_const(vm, p, "TEXTURE_2D", GL_TEXTURE_2D);
        add_const(vm, p, "TEXTURE0", GL_TEXTURE0);
        for(int i = 1; i < GL_MAX_TEXTURE_UNITS; ++i) {
            char nm[16]; snprintf(nm, sizeof(nm), "TEXTURE%d", i);
            add_const(vm, p, nm, GL_TEXTURE0 + i);
        }
        add_const(vm, p, "NEAREST", GL_NEAREST);
        add_const(vm, p, "LINEAR", GL_LINEAR);
        add_const(vm, p, "NEAREST_MIPMAP_NEAREST", GL_NEAREST_MIPMAP_NEAREST);
        add_const(vm, p, "LINEAR_MIPMAP_NEAREST", GL_LINEAR_MIPMAP_NEAREST);
        add_const(vm, p, "NEAREST_MIPMAP_LINEAR", GL_NEAREST_MIPMAP_LINEAR);
        add_const(vm, p, "LINEAR_MIPMAP_LINEAR", GL_LINEAR_MIPMAP_LINEAR);
        add_const(vm, p, "TEXTURE_MAG_FILTER", GL_TEXTURE_MAG_FILTER);
        add_const(vm, p, "TEXTURE_MIN_FILTER", GL_TEXTURE_MIN_FILTER);
        add_const(vm, p, "TEXTURE_WRAP_S", GL_TEXTURE_WRAP_S);
        add_const(vm, p, "TEXTURE_WRAP_T", GL_TEXTURE_WRAP_T);
        add_const(vm, p, "REPEAT", GL_REPEAT);
        add_const(vm, p, "CLAMP_TO_EDGE", GL_CLAMP_TO_EDGE);
        add_const(vm, p, "MIRRORED_REPEAT", GL_MIRRORED_REPEAT);
        add_const(vm, p, "RGBA", GL_RGBA);
        add_const(vm, p, "RGB", GL_RGB);
        add_const(vm, p, "LUMINANCE", GL_LUMINANCE);
        add_const(vm, p, "ALPHA", GL_ALPHA);
        add_const(vm, p, "LUMINANCE_ALPHA", GL_LUMINANCE_ALPHA);
        add_const(vm, p, "UNPACK_ALIGNMENT", GL_UNPACK_ALIGNMENT);
        add_const(vm, p, "TEXTURE_BINDING_2D", GL_TEXTURE_BINDING_2D);
        add_const(vm, p, "FRAMEBUFFER", GL_FRAMEBUFFER);
        add_const(vm, p, "RENDERBUFFER", GL_RENDERBUFFER);
        add_const(vm, p, "COLOR_ATTACHMENT0", GL_COLOR_ATTACHMENT0);
        add_const(vm, p, "DEPTH_ATTACHMENT", GL_DEPTH_ATTACHMENT);
        add_const(vm, p, "STENCIL_ATTACHMENT", GL_STENCIL_ATTACHMENT);
        add_const(vm, p, "DEPTH_COMPONENT16", GL_DEPTH_COMPONENT16);
        add_const(vm, p, "FRAMEBUFFER_COMPLETE", GL_FRAMEBUFFER_COMPLETE);
        add_const(vm, p, "FRAMEBUFFER_INCOMPLETE_ATTACHMENT", GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
        add_const(vm, p, "FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT", GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);
        add_const(vm, p, "FRAMEBUFFER_UNSUPPORTED", GL_FRAMEBUFFER_UNSUPPORTED);
        add_const(vm, p, "FRAMEBUFFER_BINDING", GL_FRAMEBUFFER_BINDING);
        add_const(vm, p, "RENDERBUFFER_BINDING", GL_RENDERBUFFER_BINDING);
        add_const(vm, p, "LOW_FLOAT", GL_LOW_FLOAT);
        add_const(vm, p, "MEDIUM_FLOAT", GL_MEDIUM_FLOAT);
        add_const(vm, p, "HIGH_FLOAT", GL_HIGH_FLOAT);
        add_const(vm, p, "LOW_INT", GL_LOW_INT);
        add_const(vm, p, "MEDIUM_INT", GL_MEDIUM_INT);
        add_const(vm, p, "HIGH_INT", GL_HIGH_INT);
    }

    g_reg_vm = vm;
    vm_reg_close(vm, gl_vm_close, vm);
    return true;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */
