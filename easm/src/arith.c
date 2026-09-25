// easm numeric instruction execution (scalar semantics, wasm-exact)
#include "easm.h"
#include "opcodes.h"
#include <math.h>

#define TRAPV(code) do { ea_trap(ex, (code)); return -1; } while (0)

static inline uint32_t rotl32(uint32_t x, uint32_t c) {
    c &= 31;
    return c ? (x << c) | (x >> (32 - c)) : x;
}
static inline uint32_t rotr32(uint32_t x, uint32_t c) {
    c &= 31;
    return c ? (x >> c) | (x << (32 - c)) : x;
}
static inline uint64_t rotl64(uint64_t x, uint64_t c) {
    c &= 63;
    return c ? (x << c) | (x >> (64 - c)) : x;
}
static inline uint64_t rotr64(uint64_t x, uint64_t c) {
    c &= 63;
    return c ? (x >> c) | (x << (64 - c)) : x;
}
static inline uint32_t ctz32(uint32_t x) { return x ? __builtin_ctz(x) : 32; }
static inline uint32_t ctz64(uint64_t x) { return x ? __builtin_ctzll(x) : 64; }
static inline uint32_t clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
static inline uint32_t clz64(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
static inline uint32_t popcnt32(uint32_t x) { return __builtin_popcount(x); }
static inline uint32_t popcnt64(uint64_t x) { return __builtin_popcountll(x); }

static inline float f32_nan_result(void) {
    float r;
    const uint32_t b = 0x7FC00000u;
    memcpy(&r, &b, 4);
    return r;
}
static inline double f64_nan_result(void) {
    double r;
    const uint64_t b = 0x7FF8000000000000ull;
    memcpy(&r, &b, 8);
    return r;
}
static inline float wasm_fmin32(float a, float b) {
    if (a != a) return f32_nan_result();
    if (b != b) return f32_nan_result();
    if (a == 0.0f && b == 0.0f) {
        uint32_t ua, ub;
        memcpy(&ua, &a, 4);
        memcpy(&ub, &b, 4);
        return ((ua | ub) >> 31) ? -0.0f : 0.0f;
    }
    return a < b ? a : b;
}
static inline float wasm_fmax32(float a, float b) {
    if (a != a) return f32_nan_result();
    if (b != b) return f32_nan_result();
    if (a == 0.0f && b == 0.0f) {
        uint32_t ua, ub;
        memcpy(&ua, &a, 4);
        memcpy(&ub, &b, 4);
        return ((ua & ub) >> 31) ? -0.0f : 0.0f;
    }
    return a > b ? a : b;
}
static inline double wasm_fmin64(double a, double b) {
    if (a != a) return f64_nan_result();
    if (b != b) return f64_nan_result();
    if (a == 0.0 && b == 0.0) {
        uint64_t ua, ub;
        memcpy(&ua, &a, 8);
        memcpy(&ub, &b, 8);
        return ((ua | ub) >> 63) ? -0.0 : 0.0;
    }
    return a < b ? a : b;
}
static inline double wasm_fmax64(double a, double b) {
    if (a != a) return f64_nan_result();
    if (b != b) return f64_nan_result();
    if (a == 0.0 && b == 0.0) {
        uint64_t ua, ub;
        memcpy(&ua, &a, 8);
        memcpy(&ub, &b, 8);
        return ((ua & ub) >> 63) ? -0.0 : 0.0;
    }
    return a > b ? a : b;
}
static inline float wasm_roundeven32(float f) { return nearbyintf(f); }
static inline double wasm_roundeven64(double f) { return nearbyint(f); }

// returns 1 handled, 0 unknown, -1 trap
int exec_numeric(EaExec *ex, uint32_t op, WVal *S, uint32_t *spp) {
    uint32_t sp = *spp;
    switch (op) {
    // ---- i32 comparison
    case EA_OP_I32_EQZ: sp--; S[sp].i32 = (S[sp].i32 == 0); sp++; *spp = sp; return 1;
    case EA_OP_I32_EQ: sp -= 2; S[sp].i32 = (S[sp].i32 == S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_NE: sp -= 2; S[sp].i32 = (S[sp].i32 != S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_LT_S: sp -= 2; S[sp].i32 = ((int32_t)S[sp].i32 < (int32_t)S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_LT_U: sp -= 2; S[sp].i32 = (S[sp].i32 < S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_GT_S: sp -= 2; S[sp].i32 = ((int32_t)S[sp].i32 > (int32_t)S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_GT_U: sp -= 2; S[sp].i32 = (S[sp].i32 > S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_LE_S: sp -= 2; S[sp].i32 = ((int32_t)S[sp].i32 <= (int32_t)S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_LE_U: sp -= 2; S[sp].i32 = (S[sp].i32 <= S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_GE_S: sp -= 2; S[sp].i32 = ((int32_t)S[sp].i32 >= (int32_t)S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_GE_U: sp -= 2; S[sp].i32 = (S[sp].i32 >= S[sp+1].i32); sp++; *spp = sp; return 1;
    // ---- i64 comparison
    case EA_OP_I64_EQZ: sp--; S[sp].i32 = (S[sp].i64 == 0); sp++; *spp = sp; return 1;
    case EA_OP_I64_EQ: sp -= 2; S[sp].i32 = (S[sp].i64 == S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_NE: sp -= 2; S[sp].i32 = (S[sp].i64 != S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_LT_S: sp -= 2; S[sp].i32 = ((int64_t)S[sp].i64 < (int64_t)S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_LT_U: sp -= 2; S[sp].i32 = (S[sp].i64 < S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_GT_S: sp -= 2; S[sp].i32 = ((int64_t)S[sp].i64 > (int64_t)S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_GT_U: sp -= 2; S[sp].i32 = (S[sp].i64 > S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_LE_S: sp -= 2; S[sp].i32 = ((int64_t)S[sp].i64 <= (int64_t)S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_LE_U: sp -= 2; S[sp].i32 = (S[sp].i64 <= S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_GE_S: sp -= 2; S[sp].i32 = ((int64_t)S[sp].i64 >= (int64_t)S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_GE_U: sp -= 2; S[sp].i32 = (S[sp].i64 >= S[sp+1].i64); sp++; *spp = sp; return 1;
    // ---- f32 comparison
    case EA_OP_F32_EQ: sp -= 2; S[sp].i32 = (S[sp].f32 == S[sp+1].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_NE: sp -= 2; S[sp].i32 = (S[sp].f32 != S[sp+1].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_LT: sp -= 2; S[sp].i32 = (S[sp].f32 < S[sp+1].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_GT: sp -= 2; S[sp].i32 = (S[sp].f32 > S[sp+1].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_LE: sp -= 2; S[sp].i32 = (S[sp].f32 <= S[sp+1].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_GE: sp -= 2; S[sp].i32 = (S[sp].f32 >= S[sp+1].f32); sp++; *spp = sp; return 1;
    // ---- f64 comparison
    case EA_OP_F64_EQ: sp -= 2; S[sp].i32 = (S[sp].f64 == S[sp+1].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_NE: sp -= 2; S[sp].i32 = (S[sp].f64 != S[sp+1].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_LT: sp -= 2; S[sp].i32 = (S[sp].f64 < S[sp+1].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_GT: sp -= 2; S[sp].i32 = (S[sp].f64 > S[sp+1].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_LE: sp -= 2; S[sp].i32 = (S[sp].f64 <= S[sp+1].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_GE: sp -= 2; S[sp].i32 = (S[sp].f64 >= S[sp+1].f64); sp++; *spp = sp; return 1;
    // ---- i32 arith
    case EA_OP_I32_CLZ: sp--; S[sp].i32 = clz32(S[sp].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_CTZ: sp--; S[sp].i32 = ctz32(S[sp].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_POPCNT: sp--; S[sp].i32 = popcnt32(S[sp].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_ADD: sp -= 2; S[sp].i32 = (uint32_t)(S[sp].i32 + S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_SUB: sp -= 2; S[sp].i32 = (uint32_t)(S[sp].i32 - S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_MUL: sp -= 2; S[sp].i32 = (uint32_t)(S[sp].i32 * S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_DIV_S: {
        sp -= 2;
        int32_t a = (int32_t)S[sp].i32, b = (int32_t)S[sp+1].i32;
        if (b == 0) TRAPV(TRAP_DIV_BY_ZERO);
        if (a == INT32_MIN && b == -1) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i32 = (uint32_t)(a / b);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I32_DIV_U:
        sp -= 2;
        if (S[sp+1].i32 == 0) TRAPV(TRAP_DIV_BY_ZERO);
        S[sp].i32 = S[sp].i32 / S[sp+1].i32;
        sp++;
        *spp = sp; return 1;
    case EA_OP_I32_REM_S: {
        sp -= 2;
        int32_t a = (int32_t)S[sp].i32, b = (int32_t)S[sp+1].i32;
        if (b == 0) TRAPV(TRAP_DIV_BY_ZERO);
        S[sp].i32 = (uint32_t)((a == INT32_MIN && b == -1) ? 0 : a % b);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I32_REM_U:
        sp -= 2;
        if (S[sp+1].i32 == 0) TRAPV(TRAP_DIV_BY_ZERO);
        S[sp].i32 = S[sp].i32 % S[sp+1].i32;
        sp++;
        *spp = sp; return 1;
    case EA_OP_I32_AND: sp -= 2; S[sp].i32 = S[sp].i32 & S[sp+1].i32; sp++; *spp = sp; return 1;
    case EA_OP_I32_OR: sp -= 2; S[sp].i32 = S[sp].i32 | S[sp+1].i32; sp++; *spp = sp; return 1;
    case EA_OP_I32_XOR: sp -= 2; S[sp].i32 = S[sp].i32 ^ S[sp+1].i32; sp++; *spp = sp; return 1;
    case EA_OP_I32_SHL: sp -= 2; S[sp].i32 = S[sp].i32 << (S[sp+1].i32 & 31); sp++; *spp = sp; return 1;
    case EA_OP_I32_SHR_S: sp -= 2; S[sp].i32 = (uint32_t)((int32_t)S[sp].i32 >> (S[sp+1].i32 & 31)); sp++; *spp = sp; return 1;
    case EA_OP_I32_SHR_U: sp -= 2; S[sp].i32 = S[sp].i32 >> (S[sp+1].i32 & 31); sp++; *spp = sp; return 1;
    case EA_OP_I32_ROTL: sp -= 2; S[sp].i32 = rotl32(S[sp].i32, S[sp+1].i32); sp++; *spp = sp; return 1;
    case EA_OP_I32_ROTR: sp -= 2; S[sp].i32 = rotr32(S[sp].i32, S[sp+1].i32); sp++; *spp = sp; return 1;
    // ---- i64 arith
    case EA_OP_I64_CLZ: sp--; S[sp].i64 = clz64(S[sp].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_CTZ: sp--; S[sp].i64 = ctz64(S[sp].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_POPCNT: sp--; S[sp].i64 = popcnt64(S[sp].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_ADD: sp -= 2; S[sp].i64 = S[sp].i64 + S[sp+1].i64; sp++; *spp = sp; return 1;
    case EA_OP_I64_SUB: sp -= 2; S[sp].i64 = S[sp].i64 - S[sp+1].i64; sp++; *spp = sp; return 1;
    case EA_OP_I64_MUL: sp -= 2; S[sp].i64 = S[sp].i64 * S[sp+1].i64; sp++; *spp = sp; return 1;
    case EA_OP_I64_DIV_S: {
        sp -= 2;
        int64_t a = (int64_t)S[sp].i64, b = (int64_t)S[sp+1].i64;
        if (b == 0) TRAPV(TRAP_DIV_BY_ZERO);
        if (a == INT64_MIN && b == -1) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i64 = (uint64_t)(a / b);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_DIV_U:
        sp -= 2;
        if (S[sp+1].i64 == 0) TRAPV(TRAP_DIV_BY_ZERO);
        S[sp].i64 = S[sp].i64 / S[sp+1].i64;
        sp++;
        *spp = sp; return 1;
    case EA_OP_I64_REM_S: {
        sp -= 2;
        int64_t a = (int64_t)S[sp].i64, b = (int64_t)S[sp+1].i64;
        if (b == 0) TRAPV(TRAP_DIV_BY_ZERO);
        S[sp].i64 = (uint64_t)((a == INT64_MIN && b == -1) ? 0 : a % b);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_REM_U:
        sp -= 2;
        if (S[sp+1].i64 == 0) TRAPV(TRAP_DIV_BY_ZERO);
        S[sp].i64 = S[sp].i64 % S[sp+1].i64;
        sp++;
        *spp = sp; return 1;
    case EA_OP_I64_AND: sp -= 2; S[sp].i64 = S[sp].i64 & S[sp+1].i64; sp++; *spp = sp; return 1;
    case EA_OP_I64_OR: sp -= 2; S[sp].i64 = S[sp].i64 | S[sp+1].i64; sp++; *spp = sp; return 1;
    case EA_OP_I64_XOR: sp -= 2; S[sp].i64 = S[sp].i64 ^ S[sp+1].i64; sp++; *spp = sp; return 1;
    case EA_OP_I64_SHL: sp -= 2; S[sp].i64 = S[sp].i64 << (S[sp+1].i64 & 63); sp++; *spp = sp; return 1;
    case EA_OP_I64_SHR_S: sp -= 2; S[sp].i64 = (uint64_t)((int64_t)S[sp].i64 >> (S[sp+1].i64 & 63)); sp++; *spp = sp; return 1;
    case EA_OP_I64_SHR_U: sp -= 2; S[sp].i64 = S[sp].i64 >> (S[sp+1].i64 & 63); sp++; *spp = sp; return 1;
    case EA_OP_I64_ROTL: sp -= 2; S[sp].i64 = rotl64(S[sp].i64, S[sp+1].i64); sp++; *spp = sp; return 1;
    case EA_OP_I64_ROTR: sp -= 2; S[sp].i64 = rotr64(S[sp].i64, S[sp+1].i64); sp++; *spp = sp; return 1;
    // ---- f32 arith
    case EA_OP_F32_ABS: {
        uint32_t b;
        sp--;
        memcpy(&b, &S[sp].f32, 4);
        b &= 0x7FFFFFFFu;
        memcpy(&S[sp].f32, &b, 4);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_F32_NEG: {
        uint32_t b;
        sp--;
        memcpy(&b, &S[sp].f32, 4);
        b ^= 0x80000000u;
        memcpy(&S[sp].f32, &b, 4);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_F32_CEIL: sp--; S[sp].f32 = ceilf(S[sp].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_FLOOR: sp--; S[sp].f32 = floorf(S[sp].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_TRUNC: sp--; S[sp].f32 = truncf(S[sp].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_NEAREST: sp--; S[sp].f32 = wasm_roundeven32(S[sp].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_SQRT: sp--; S[sp].f32 = S[sp].f32 < 0 ? f32_nan_result() : sqrtf(S[sp].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_ADD: sp -= 2; S[sp].f32 = S[sp].f32 + S[sp+1].f32; sp++; *spp = sp; return 1;
    case EA_OP_F32_SUB: sp -= 2; S[sp].f32 = S[sp].f32 - S[sp+1].f32; sp++; *spp = sp; return 1;
    case EA_OP_F32_MUL: sp -= 2; S[sp].f32 = S[sp].f32 * S[sp+1].f32; sp++; *spp = sp; return 1;
    case EA_OP_F32_DIV: sp -= 2; S[sp].f32 = S[sp].f32 / S[sp+1].f32; sp++; *spp = sp; return 1;
    case EA_OP_F32_MIN: sp -= 2; S[sp].f32 = wasm_fmin32(S[sp].f32, S[sp+1].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_MAX: sp -= 2; S[sp].f32 = wasm_fmax32(S[sp].f32, S[sp+1].f32); sp++; *spp = sp; return 1;
    case EA_OP_F32_COPYSIGN: {
        uint32_t a, b;
        sp -= 2;
        memcpy(&a, &S[sp].f32, 4);
        memcpy(&b, &S[sp+1].f32, 4);
        a = (a & 0x7FFFFFFFu) | (b & 0x80000000u);
        memcpy(&S[sp].f32, &a, 4);
        sp++;
        *spp = sp; return 1;
    }
    // ---- f64 arith
    case EA_OP_F64_ABS: {
        uint64_t b;
        sp--;
        memcpy(&b, &S[sp].f64, 8);
        b &= 0x7FFFFFFFFFFFFFFFull;
        memcpy(&S[sp].f64, &b, 8);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_F64_NEG: {
        uint64_t b;
        sp--;
        memcpy(&b, &S[sp].f64, 8);
        b ^= 0x8000000000000000ull;
        memcpy(&S[sp].f64, &b, 8);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_F64_CEIL: sp--; S[sp].f64 = ceil(S[sp].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_FLOOR: sp--; S[sp].f64 = floor(S[sp].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_TRUNC: sp--; S[sp].f64 = trunc(S[sp].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_NEAREST: sp--; S[sp].f64 = wasm_roundeven64(S[sp].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_SQRT: sp--; S[sp].f64 = S[sp].f64 < 0 ? f64_nan_result() : sqrt(S[sp].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_ADD: sp -= 2; S[sp].f64 = S[sp].f64 + S[sp+1].f64; sp++; *spp = sp; return 1;
    case EA_OP_F64_SUB: sp -= 2; S[sp].f64 = S[sp].f64 - S[sp+1].f64; sp++; *spp = sp; return 1;
    case EA_OP_F64_MUL: sp -= 2; S[sp].f64 = S[sp].f64 * S[sp+1].f64; sp++; *spp = sp; return 1;
    case EA_OP_F64_DIV: sp -= 2; S[sp].f64 = S[sp].f64 / S[sp+1].f64; sp++; *spp = sp; return 1;
    case EA_OP_F64_MIN: sp -= 2; S[sp].f64 = wasm_fmin64(S[sp].f64, S[sp+1].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_MAX: sp -= 2; S[sp].f64 = wasm_fmax64(S[sp].f64, S[sp+1].f64); sp++; *spp = sp; return 1;
    case EA_OP_F64_COPYSIGN: {
        uint64_t a, b;
        sp -= 2;
        memcpy(&a, &S[sp].f64, 8);
        memcpy(&b, &S[sp+1].f64, 8);
        a = (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull);
        memcpy(&S[sp].f64, &a, 8);
        sp++;
        *spp = sp; return 1;
    }
    // ---- conversions
    case EA_OP_I32_WRAP_I64: sp--; S[sp].i32 = (uint32_t)S[sp].i64; sp++; *spp = sp; return 1;
    case EA_OP_I32_TRUNC_F32_S: {
        float f = S[--sp].f32;
        if (f != f) TRAPV(TRAP_INVALID_CONV);
        if (f >= 2147483648.0f || f < -2147483648.0f) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i32 = (uint32_t)(int32_t)truncf(f);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I32_TRUNC_F32_U: {
        float f = S[--sp].f32;
        if (f != f) TRAPV(TRAP_INVALID_CONV);
        if (f >= 4294967296.0f || f <= -1.0f) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i32 = (uint32_t)truncf(f);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I32_TRUNC_F64_S: {
        double f = S[--sp].f64;
        if (f != f) TRAPV(TRAP_INVALID_CONV);
        if (f >= 2147483648.0 || f <= -2147483649.0) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i32 = (uint32_t)(int32_t)trunc(f);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I32_TRUNC_F64_U: {
        double f = S[--sp].f64;
        if (f != f) TRAPV(TRAP_INVALID_CONV);
        if (f >= 4294967296.0 || f <= -1.0) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i32 = (uint32_t)trunc(f);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_EXTEND_I32_S: sp--; S[sp].i64 = (uint64_t)(int64_t)(int32_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_I64_EXTEND_I32_U: sp--; S[sp].i64 = (uint64_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_I64_TRUNC_F32_S: {
        float f = S[--sp].f32;
        if (f != f) TRAPV(TRAP_INVALID_CONV);
        if (f >= 9223372036854775808.0f || f < -9223372036854775808.0f) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i64 = (uint64_t)(int64_t)truncf(f);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_TRUNC_F32_U: {
        float f = S[--sp].f32;
        if (f != f) TRAPV(TRAP_INVALID_CONV);
        if (f >= 18446744073709551616.0f || f <= -1.0f) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i64 = (uint64_t)truncf(f);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_TRUNC_F64_S: {
        double f = S[--sp].f64;
        if (f != f) TRAPV(TRAP_INVALID_CONV);
        if (f >= 9223372036854775808.0 || f < -9223372036854775808.0) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i64 = (uint64_t)(int64_t)trunc(f);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_TRUNC_F64_U: {
        double f = S[--sp].f64;
        if (f != f) TRAPV(TRAP_INVALID_CONV);
        if (f >= 18446744073709551616.0 || f <= -1.0) TRAPV(TRAP_INT_OVERFLOW);
        S[sp].i64 = (uint64_t)trunc(f);
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_F32_CONVERT_I32_S: sp--; S[sp].f32 = (float)(int32_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_F32_CONVERT_I32_U: sp--; S[sp].f32 = (float)(uint32_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_F32_CONVERT_I64_S: sp--; S[sp].f32 = (float)(int64_t)S[sp].i64; sp++; *spp = sp; return 1;
    case EA_OP_F32_CONVERT_I64_U: sp--; S[sp].f32 = (float)(uint64_t)S[sp].i64; sp++; *spp = sp; return 1;
    case EA_OP_F32_DEMOTE_F64: sp--; S[sp].f32 = (float)S[sp].f64; sp++; *spp = sp; return 1;
    case EA_OP_F64_CONVERT_I32_S: sp--; S[sp].f64 = (double)(int32_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_F64_CONVERT_I32_U: sp--; S[sp].f64 = (double)(uint32_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_F64_CONVERT_I64_S: sp--; S[sp].f64 = (double)(int64_t)S[sp].i64; sp++; *spp = sp; return 1;
    case EA_OP_F64_CONVERT_I64_U: sp--; S[sp].f64 = (double)(uint64_t)S[sp].i64; sp++; *spp = sp; return 1;
    case EA_OP_F64_PROMOTE_F32: sp--; S[sp].f64 = (double)S[sp].f32; sp++; *spp = sp; return 1;
    // reinterpretations: pure bit moves (WVal is a union, value stays in place)
    case EA_OP_I32_REINTERPRET_F32: case EA_OP_F32_REINTERPRET_I32:
    case EA_OP_I64_REINTERPRET_F64: case EA_OP_F64_REINTERPRET_I64:
        *spp = sp; return 1;
    default:
        break;
    }
    // sign extensions
    switch (op) {
    case EA_OP_I32_EXTEND8_S: sp--; S[sp].i32 = (uint32_t)(int32_t)(int8_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_I32_EXTEND16_S: sp--; S[sp].i32 = (uint32_t)(int32_t)(int16_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_I64_EXTEND8_S: sp--; S[sp].i64 = (uint64_t)(int64_t)(int8_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_I64_EXTEND16_S: sp--; S[sp].i64 = (uint64_t)(int64_t)(int16_t)S[sp].i32; sp++; *spp = sp; return 1;
    case EA_OP_I64_EXTEND32_S: sp--; S[sp].i64 = (uint64_t)(int64_t)(int32_t)S[sp].i32; sp++; *spp = sp; return 1;
    // trunc_sat
    case EA_OP_I32_TRUNC_SAT_F32_S: {
        float f = S[--sp].f32;
        S[sp].i32 = f != f ? 0 : (f >= 2147483648.0f ? 0x7FFFFFFFu :
                     (f < -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)truncf(f)));
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I32_TRUNC_SAT_F32_U: {
        float f = S[--sp].f32;
        S[sp].i32 = f != f ? 0 : (f >= 4294967296.0f ? 0xFFFFFFFFu :
                     (f <= -1.0f ? 0 : (uint32_t)truncf(f)));
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I32_TRUNC_SAT_F64_S: {
        double f = S[--sp].f64;
        S[sp].i32 = f != f ? 0 : (f >= 2147483648.0 ? 0x7FFFFFFFu :
                     (f < -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)trunc(f)));
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I32_TRUNC_SAT_F64_U: {
        double f = S[--sp].f64;
        S[sp].i32 = f != f ? 0 : (f >= 4294967296.0 ? 0xFFFFFFFFu :
                     (f <= -1.0 ? 0 : (uint32_t)trunc(f)));
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_TRUNC_SAT_F32_S: {
        float f = S[--sp].f32;
        S[sp].i64 = f != f ? 0 : (f >= 9223372036854775808.0f ? 0x7FFFFFFFFFFFFFFFull :
                     (f < -9223372036854775808.0f ? 0x8000000000000000ull : (uint64_t)(int64_t)truncf(f)));
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_TRUNC_SAT_F32_U: {
        float f = S[--sp].f32;
        S[sp].i64 = f != f ? 0 : (f >= 18446744073709551616.0f ? 0xFFFFFFFFFFFFFFFFull :
                     (f <= -1.0f ? 0 : (uint64_t)truncf(f)));
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_TRUNC_SAT_F64_S: {
        double f = S[--sp].f64;
        S[sp].i64 = f != f ? 0 : (f >= 9223372036854775808.0 ? 0x7FFFFFFFFFFFFFFFull :
                     (f < -9223372036854775808.0 ? 0x8000000000000000ull : (uint64_t)(int64_t)trunc(f)));
        sp++;
        *spp = sp; return 1;
    }
    case EA_OP_I64_TRUNC_SAT_F64_U: {
        double f = S[--sp].f64;
        S[sp].i64 = f != f ? 0 : (f >= 18446744073709551616.0 ? 0xFFFFFFFFFFFFFFFFull :
                     (f <= -1.0 ? 0 : (uint64_t)trunc(f)));
        sp++;
        *spp = sp; return 1;
    }
    default:
        return 0;
    }
}
