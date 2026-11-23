/*
 * js_glsl.h - a minimal GLSL ES 1.00 subset interpreter.
 *
 * This is PURE C: it knows nothing about mario, the DOM, or WebGL state. It
 * compiles a vertex or fragment shader source into a small AST and executes
 * main() once per vertex / fragment. All interaction with the surrounding GL
 * state (attribute buffers, uniforms, varying storage, gl_Position /
 * gl_FragColor outputs) happens through two callbacks the caller supplies, so
 * the interpreter never owns any GL state itself.
 *
 * Supported subset (enough for real WebGL fingerprint / demo shaders, and a
 * genuinely useful general core):
 *   - types: float, int, bool, vec2, vec3, vec4 (ints/bools are stored as
 *     float scalars; sufficient for arithmetic + comparison in this subset)
 *   - qualifiers: attribute, varying, uniform, const (interface reflection),
 *     precision statements (parsed and ignored)
 *   - constructors: float()/int()/bool()/vec2()/vec3()/vec4() with the standard
 *     GLSL "fill components left to right, scalar broadcasts" rule
 *   - swizzles: .x .y .z .w and .r .g .b .a (read, and read-modify-write on
 *     assignment targets)
 *   - operators: + - * / unary- , == != < > <= >= && || ! , ternary ?:,
 *     compound assignment += -= *= /=
 *     (component-wise for vec-vec, broadcast for vec-scalar, per GLSL)
 *   - builtins: radians degrees sin cos tan asin acos atan sinh.. (trig),
 *     abs sign floor ceil fract mod sqrt pow exp log exp2 log2 inversesqrt,
 *     min max clamp mix step smoothstep length distance dot cross normalize,
 *     reflect (scalar + vector forms), texture2D (returns vec4(0) - no sampler)
 *   - statements: declaration (with optional initializer), assignment,
 *     expression statement, block, if/else, for, while, return, discard
 *
 * Deliberately unsupported (compile fails honestly, GL reports COMPILE_STATUS
 * false): structs, arrays, matrices, user functions, samplers with real
 * filtering, #preprocessor. A shader needing these simply does not compile -
 * which is the correct WebGL behaviour, not a crash.
 */

#ifndef MARIO_JS_GLSL_H
#define MARIO_JS_GLSL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shader stage passed to glsl_compile. */
#define GLSL_STAGE_VERTEX   0
#define GLSL_STAGE_FRAGMENT 1

/* Interface-variable kinds reflected out of a compiled shader. */
#define GLSL_VAR_ATTRIB   0
#define GLSL_VAR_VARYING  1
#define GLSL_VAR_UNIFORM  2

/* A GLSL value: 1..4 float components (n==1 is a scalar float/int/bool). */
typedef struct {
    int   n;        /* component count: 1, 2, 3 or 4 */
    float c[4];     /* components (only the first n are meaningful) */
} glsl_val;

/* Opaque compiled shader. */
typedef struct glsl_shader glsl_shader;

/* Read an input by name (attribute / uniform / varying-in / gl_FragCoord ...).
 * Return 1 and fill *out when the name is a known input; return 0 to let the
 * interpreter substitute a zero value of the appropriate shape. */
typedef int (*glsl_getvar_fn)(void* ud, const char* name, glsl_val* out);

/* Write an output by name (gl_Position / gl_FragColor / gl_PointSize /
 * varying-out). Also used for read-modify-write of a swizzled interface var. */
typedef void (*glsl_setvar_fn)(void* ud, const char* name, const glsl_val* v);

/* Compile GLSL `src` for `stage`. Returns an opaque shader, or NULL if the
 * source uses an unsupported construct / fails to parse (the honest "shader
 * did not compile" signal - the GL layer turns this into COMPILE_STATUS false).
 * `err` (optional, may be NULL) receives a mario_malloc'd message on failure. */
glsl_shader* glsl_compile(const char* src, int stage, char** err);

/* Free a compiled shader (NULL-safe). */
void glsl_free(glsl_shader* s);

/* Number of interface variables of `kind` (GLSL_VAR_*), and the name of the
 * i-th one. Used by the GL layer to hand out attribute / uniform locations. */
int         glsl_var_count(glsl_shader* s, int kind);
const char* glsl_var_name(glsl_shader* s, int kind, int i);

/* Execute main() once. `ud`, `getv` and `setv` mediate every read/write of
 * interface variables and built-in inputs/outputs. Returns 1 on normal
 * completion, 0 if the shader executed `discard` (fragment stage). */
int glsl_run(glsl_shader* s, void* ud, glsl_getvar_fn getv, glsl_setvar_fn setv);

#ifdef __cplusplus
}
#endif

#endif /* MARIO_JS_GLSL_H */
