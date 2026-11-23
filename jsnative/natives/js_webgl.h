/*
 * js_webgl.h - a software WebGL 1.0 (OpenGL ES 2.0) backend for the mario VM.
 *
 * ewebview historically had NO WebGL implementation: canvas.getContext('webgl')
 * honestly returned null. That is a genuine Web-standard gap - every real
 * browser (Chrome/Safari/Firefox) exposes a WebGLRenderingContext, and a large
 * class of pages (device-fingerprint SDKs, WebGL demos, canvas-based charts)
 * depends on it. This module closes that gap with a REAL software renderer: a
 * GLSL ES 1.00 interpreter (js_glsl.c) drives per-vertex / per-fragment
 * shading, a CPU rasterizer covers the WebGL primitive modes, and the resulting
 * framebuffer is pushed into the SAME embedder canvas the 2D context uses, so
 * canvas.toDataURL() reads back the genuinely rendered pixels with no changes
 * to the canvas bridge.
 *
 * HONESTY (a hard red line): the renderer reports its true identity. VENDOR /
 * RENDERER use the WebGL-spec masked defaults ("WebKit" / "WebKit WebGL");
 * UNMASKED_VENDOR_WEBGL / UNMASKED_RENDERER_WEBGL (via WEBGL_debug_renderer_info)
 * report the real software rasterizer, NOT a fabricated GPU string. We never
 * claim to be a specific graphics card. A shader using an unsupported GLSL
 * construct fails to compile honestly (COMPILE_STATUS false), exactly as a real
 * driver would.
 *
 * Relationship to js_canvas.c: js_register_canvas_natives() calls
 * js_webgl_register(vm) once to build + pin the WebGL classes, then the
 * getContext('webgl'|'experimental-webgl'|...) branch calls
 * js_webgl_create_context() with the embedder canvas handle and the set_pixel
 * callback it already uses for the 2D path. This header depends only on mario.h
 * so it can be included from js_canvas.c without circular includes.
 */

#ifndef MARIO_JS_WEBGL_H
#define MARIO_JS_WEBGL_H

#include "mario.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches js_canvas_callbacks_t.set_pixel: push one 0xAARRGGBB pixel into the
 * embedder-owned canvas at device (x,y). The WebGL framebuffer is flushed
 * through this so toDataURL()/compositing see the rendered result. */
typedef void (*webgl_set_pixel_fn)(void* ctx, void* canvas, int x, int y, uint32_t color);

/* Build and pin the WebGL classes (WebGLRenderingContext, WebGLBuffer,
 * WebGLShader, WebGLProgram, WebGLUniformLocation, WebGLActiveInfo) on vm,
 * registering every native method, accessor and numeric constant. Idempotent:
 * safe to call more than once (only the first call registers). Must run AFTER
 * vm_init() and BEFORE any getContext('webgl'). Returns true on success. */
bool js_webgl_register(vm_t* vm);

/* Create a WebGLRenderingContext bound to a w x h framebuffer that flushes into
 * `canvas` (the embedder handle from canvas_create) via `setpx`. `embed_ctx` is
 * passed verbatim to setpx. `canvas_el` (may be NULL) is the <canvas> Element
 * var returned by gl.canvas; it is ref'd for the context's lifetime. Returns the
 * context object, or var_new_null(vm) if allocation failed. */
var_t* js_webgl_create_context(vm_t* vm, int w, int h,
                               void* embed_ctx, void* canvas,
                               webgl_set_pixel_fn setpx,
                               var_t* canvas_el);

#ifdef __cplusplus
}
#endif

#endif /* MARIO_JS_WEBGL_H */
