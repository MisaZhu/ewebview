// easm SIMD execution (scalar lane loops; correctness first, NEON fast path comes with JIT phase)
#include <stdio.h>
#include "easm.h"
#include "opcodes.h"
#include <math.h>

typedef union {
    uint8_t b[16];
    int8_t sb[16];
    uint16_t h[8];
    int16_t sh[8];
    uint32_t w[4];
    int32_t sw[4];
    int64_t sd[2];
    uint64_t d[2];
    float f[4];
    double g[2];
} V128U;

#define TRAPV(code) do { ea_trap(ex, (code)); return -1; } while (0)

static float simd_f32_nan(void) {
    float r;
    const uint32_t bits = 0x7FC00000u;
    memcpy(&r, &bits, 4);
    return r;
}
static double simd_f64_nan(void) {
    double r;
    const uint64_t bits = 0x7FF8000000000000ull;
    memcpy(&r, &bits, 8);
    return r;
}
static float simd_fmin32(float a, float b) {
    if (a != a || b != b) return simd_f32_nan();
    if (a == 0.0f && b == 0.0f) {
        uint32_t ua, ub;
        memcpy(&ua, &a, 4);
        memcpy(&ub, &b, 4);
        return ((ua | ub) >> 31) ? -0.0f : 0.0f;
    }
    return a < b ? a : b;
}
static float simd_fmax32(float a, float b) {
    if (a != a || b != b) return simd_f32_nan();
    if (a == 0.0f && b == 0.0f) {
        uint32_t ua, ub;
        memcpy(&ua, &a, 4);
        memcpy(&ub, &b, 4);
        return ((ua & ub) >> 31) ? -0.0f : 0.0f;
    }
    return a > b ? a : b;
}
static double simd_fmin64(double a, double b) {
    if (a != a || b != b) return simd_f64_nan();
    if (a == 0.0 && b == 0.0) {
        uint64_t ua, ub;
        memcpy(&ua, &a, 8);
        memcpy(&ub, &b, 8);
        return ((ua | ub) >> 63) ? -0.0 : 0.0;
    }
    return a < b ? a : b;
}
static double simd_fmax64(double a, double b) {
    if (a != a || b != b) return simd_f64_nan();
    if (a == 0.0 && b == 0.0) {
        uint64_t ua, ub;
        memcpy(&ua, &a, 8);
        memcpy(&ub, &b, 8);
        return ((ua & ub) >> 63) ? -0.0 : 0.0;
    }
    return a > b ? a : b;
}

#define LOAD_A  memcpy(&A, S[sp - 1].v128, 16)
#define LOAD_AB memcpy(&A, S[sp - 2].v128, 16); memcpy(&B, S[sp - 1].v128, 16)
#define LOAD_ABC memcpy(&A, S[sp - 3].v128, 16); memcpy(&B, S[sp - 2].v128, 16); memcpy(&C, S[sp - 1].v128, 16)
#define PUT_D  memcpy(S[sp - 1].v128, &D, 16); *spp = sp; return 1
#define PUT_D_DROP  memcpy(S[sp - 2].v128, &D, 16); sp--; *spp = sp; return 1
#define PUT_D_DROP2 memcpy(S[sp - 3].v128, &D, 16); sp -= 2; *spp = sp; return 1

#define BIN_I(N, T, EXPR) do { \
    LOAD_AB; \
    for (int i = 0; i < (N); i++) { T x = ((T *)(&A))[i], y = ((T *)(&B))[i]; ((T *)(&D))[i] = (T)(EXPR); } \
    PUT_D_DROP; } while (0)

#define UN_I(N, T, EXPR) do { \
    LOAD_A; \
    for (int i = 0; i < (N); i++) { T x = ((T *)(&A))[i]; ((T *)(&D))[i] = (T)(EXPR); } \
    PUT_D; } while (0)

#define CMP_I(N, T, EXPR) do { \
    LOAD_AB; \
    for (int i = 0; i < (N); i++) { T x = ((T *)(&A))[i], y = ((T *)(&B))[i]; ((T *)(&D))[i] = (EXPR) ? (T)-1 : 0; } \
    PUT_D_DROP; } while (0)

#define SHIFT_I(N, T, EXPR) do { \
    LOAD_AB; \
    uint32_t cnt = B.w[0]; \
    for (int i = 0; i < (N); i++) { T x = ((T *)(&A))[i]; ((T *)(&D))[i] = (T)(EXPR); } \
    PUT_D_DROP; } while (0)

#define BIN_F32(EXPR) do { \
    LOAD_AB; \
    for (int i = 0; i < 4; i++) { float r = (EXPR); D.w[i] = *(uint32_t *)&r; } \
    PUT_D_DROP; } while (0)

#define BIN_F64(EXPR) do { \
    LOAD_AB; \
    for (int i = 0; i < 2; i++) { double r = (EXPR); D.d[i] = *(uint64_t *)&r; } \
    PUT_D_DROP; } while (0)

#define FCMP_I(N, T, EXPR) do { \
    LOAD_AB; \
    for (int i = 0; i < (N); i++) { float x = A.f[i]; float y = B.f[i]; (void)x; (void)y; ((T *)(&D))[i] = (EXPR) ? (T)-1 : 0; } \
    PUT_D; } while (0)

static inline uint8_t sat_add_u8(uint8_t a, uint8_t b) {
    uint16_t r = (uint16_t)a + b;
    return r > 255 ? 255 : (uint8_t)r;
}
static inline int8_t sat_add_s8(int8_t a, int8_t b) {
    int16_t r = (int16_t)a + b;
    return r > 127 ? 127 : (r < -128 ? -128 : (int8_t)r);
}
static inline uint8_t sat_sub_u8(uint8_t a, uint8_t b) {
    int16_t r = (int16_t)a - b;
    return r < 0 ? 0 : (uint8_t)r;
}
static inline int8_t sat_sub_s8(int8_t a, int8_t b) {
    int16_t r = (int16_t)a - b;
    return r > 127 ? 127 : (r < -128 ? -128 : (int8_t)r);
}
static inline uint16_t sat_add_u16(uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + b;
    return r > 65535 ? 65535 : (uint16_t)r;
}
static inline int16_t sat_add_s16(int16_t a, int16_t b) {
    int32_t r = (int32_t)a + b;
    return r > 32767 ? 32767 : (r < -32768 ? -32768 : (int16_t)r);
}
static inline uint16_t sat_sub_u16(uint16_t a, uint16_t b) {
    int32_t r = (int32_t)a - b;
    return r < 0 ? 0 : (uint16_t)r;
}
static inline int16_t sat_sub_s16(int16_t a, int16_t b) {
    int32_t r = (int32_t)a - b;
    return r > 32767 ? 32767 : (r < -32768 ? -32768 : (int16_t)r);
}
static inline uint8_t avgr_u8(uint8_t a, uint8_t b) {
    return (uint8_t)(((uint32_t)a + b + 1) >> 1);
}
static inline uint16_t avgr_u16(uint16_t a, uint16_t b) {
    return (uint16_t)(((uint32_t)a + b + 1) >> 1);
}

// returns 1 handled, 0 unknown, -1 trap
int exec_simd(EaExec *ex, EaInstance *inst, const EaInstr *in, WVal *S, uint32_t *spp) {
    uint32_t op = in->opcode;
    if (op < EA_OPV_BASE) return 0; // simd (incl. relaxed) opcode space
    uint32_t sp = *spp;
    V128U A, B, C, D;
    switch (op) {
    // ================= memory ops =================
    case EA_OP_V128_STORE: {
        // stack: [i32 addr, v128]
        uint64_t ea = (uint64_t)S[sp - 2].i32 + in->imm.ma.offset;
        EaMemInst *mem = inst->memories[in->imm.ma.memidx];
        if (16 > mem->size || ea > mem->size - 16) TRAPV(TRAP_OOB_MEMORY);
        memcpy(mem->base + ea, S[sp - 1].v128, 16);
        sp -= 2;
        *spp = sp;
        return 1;
    }
    case EA_OP_V128_STORE8_LANE: case EA_OP_V128_STORE16_LANE:
    case EA_OP_V128_STORE32_LANE: case EA_OP_V128_STORE64_LANE: {
        uint64_t w = 1ull << (op - EA_OP_V128_STORE8_LANE);
        uint8_t lane = in->lane;
        uint64_t ea = (uint64_t)S[sp - 2].i32 + in->imm.ma.offset;
        EaMemInst *mem = inst->memories[in->imm.ma.memidx];
        if (w > mem->size || ea > mem->size - w) TRAPV(TRAP_OOB_MEMORY);
        uint8_t *p = mem->base + ea;
        uint8_t *v = S[sp - 1].v128;
        switch (w) {
        case 1: p[0] = v[lane]; break;
        case 2: memcpy(p, v + 2 * lane, 2); break;
        case 4: memcpy(p, v + 4 * lane, 4); break;
        case 8: memcpy(p, v + 8 * lane, 8); break;
        }
        sp -= 2;
        *spp = sp;
        return 1;
    }
    case EA_OP_V128_LOAD: case EA_OP_V128_LOAD8X8_S: case EA_OP_V128_LOAD8X8_U:
    case EA_OP_V128_LOAD16X4_S: case EA_OP_V128_LOAD16X4_U:
    case EA_OP_V128_LOAD32X2_S: case EA_OP_V128_LOAD32X2_U:
    case EA_OP_V128_LOAD8_SPLAT: case EA_OP_V128_LOAD16_SPLAT:
    case EA_OP_V128_LOAD32_SPLAT: case EA_OP_V128_LOAD64_SPLAT:
    case EA_OP_V128_LOAD32_ZERO: case EA_OP_V128_LOAD64_ZERO: {
        uint64_t w = 16;
        switch (op) {
        case EA_OP_V128_LOAD8X8_S: case EA_OP_V128_LOAD8X8_U:
        case EA_OP_V128_LOAD16X4_S: case EA_OP_V128_LOAD16X4_U:
        case EA_OP_V128_LOAD32X2_S: case EA_OP_V128_LOAD32X2_U: w = 8; break;
        case EA_OP_V128_LOAD8_SPLAT: w = 1; break;
        case EA_OP_V128_LOAD16_SPLAT: w = 2; break;
        case EA_OP_V128_LOAD32_SPLAT: case EA_OP_V128_LOAD32_ZERO: w = 4; break;
        case EA_OP_V128_LOAD64_SPLAT: case EA_OP_V128_LOAD64_ZERO: w = 8; break;
        }
        uint64_t ea = (uint64_t)S[sp - 1].i32 + in->imm.ma.offset;
        EaMemInst *mem = inst->memories[in->imm.ma.memidx];
        if (w > mem->size || ea > mem->size - w) TRAPV(TRAP_OOB_MEMORY);
        uint8_t *p = mem->base + ea;
        memset(&D, 0, 16);
        switch (op) {
        case EA_OP_V128_LOAD: memcpy(&D, p, 16); break;
        case EA_OP_V128_LOAD8X8_S:
            for (int i = 0; i < 8; i++) D.sh[i] = (int16_t)(int8_t)p[i];
            break;
        case EA_OP_V128_LOAD8X8_U:
            for (int i = 0; i < 8; i++) D.h[i] = p[i];
            break;
        case EA_OP_V128_LOAD16X4_S: {
            int16_t t[4];
            memcpy(t, p, 8);
            for (int i = 0; i < 4; i++) D.sw[i] = (int32_t)t[i];
            break;
        }
        case EA_OP_V128_LOAD16X4_U: {
            uint16_t t[4];
            memcpy(t, p, 8);
            for (int i = 0; i < 4; i++) D.w[i] = t[i];
            break;
        }
        case EA_OP_V128_LOAD32X2_S: {
            int32_t t[2];
            memcpy(t, p, 8);
            for (int i = 0; i < 2; i++) D.sd[i] = (int64_t)t[i];
            break;
        }
        case EA_OP_V128_LOAD32X2_U: {
            uint32_t t[2];
            memcpy(t, p, 8);
            for (int i = 0; i < 2; i++) D.d[i] = t[i];
            break;
        }
        case EA_OP_V128_LOAD8_SPLAT: memset(&D, p[0], 16); break;
        case EA_OP_V128_LOAD16_SPLAT: {
            uint16_t v;
            memcpy(&v, p, 2);
            for (int i = 0; i < 8; i++) D.h[i] = v;
            break;
        }
        case EA_OP_V128_LOAD32_SPLAT: {
            uint32_t v;
            memcpy(&v, p, 4);
            for (int i = 0; i < 4; i++) D.w[i] = v;
            break;
        }
        case EA_OP_V128_LOAD64_SPLAT: {
            uint64_t v;
            memcpy(&v, p, 8);
            D.d[0] = D.d[1] = v;
            break;
        }
        case EA_OP_V128_LOAD32_ZERO: memcpy(&D.w[0], p, 4); break;
        case EA_OP_V128_LOAD64_ZERO: memcpy(&D.d[0], p, 8); break;
        }
        memcpy(S[sp - 1].v128, &D, 16);
        *spp = sp;
        return 1;
    }
    case EA_OP_V128_LOAD8_LANE: case EA_OP_V128_LOAD16_LANE:
    case EA_OP_V128_LOAD32_LANE: case EA_OP_V128_LOAD64_LANE: {
        // stack: [i32 addr, v128 vec] -> v128
        uint64_t w = 1ull << (op - EA_OP_V128_LOAD8_LANE);
        uint8_t lane = in->lane;
        uint64_t ea = (uint64_t)S[sp - 2].i32 + in->imm.ma.offset;
        EaMemInst *mem = inst->memories[in->imm.ma.memidx];
        if (w > mem->size || ea > mem->size - w) TRAPV(TRAP_OOB_MEMORY);
        uint8_t *p = mem->base + ea;
        // result replaces the pair: copy vec into the addr slot, update lane, drop one slot
        uint8_t *v = S[sp - 2].v128;
        memmove(v, S[sp - 1].v128, 16);
        switch (w) {
        case 1: v[lane] = p[0]; break;
        case 2: memcpy(v + 2 * lane, p, 2); break;
        case 4: memcpy(v + 4 * lane, p, 4); break;
        case 8: memcpy(v + 8 * lane, p, 8); break;
        }
        sp--;
        *spp = sp;
        return 1;
    }
    case EA_OP_V128_CONST:
        S[sp] = (WVal){0};
        memcpy(S[sp].v128, in->imm.bytes, 16);
        sp++;
        *spp = sp;
        return 1;
    // ================= shuffles / splats / lanes =================
    case EA_OP_I8X16_SHUFFLE: {
        LOAD_AB;
        const uint8_t *idx = in->imm.bytes;
        for (int i = 0; i < 16; i++) {
            uint8_t k = idx[i];
            D.b[i] = k < 16 ? A.b[k] : B.b[k - 16];
        }
        PUT_D_DROP;
    }
    case EA_OP_I8X16_SWIZZLE: {
        LOAD_AB;
        for (int i = 0; i < 16; i++) {
            uint8_t k = B.b[i];
            D.b[i] = k < 16 ? A.b[k] : 0;
        }
        PUT_D_DROP;
    }
    case EA_OP_I8X16_SPLAT: { uint8_t v = (uint8_t)S[--sp].i32; memset(&D, v, 16); S[sp] = (WVal){0}; memcpy(S[sp].v128, &D, 16); sp++; *spp = sp; return 1; }
    case EA_OP_I16X8_SPLAT: { uint16_t v = (uint16_t)S[--sp].i32; for (int i = 0; i < 8; i++) D.h[i] = v; S[sp] = (WVal){0}; memcpy(S[sp].v128, &D, 16); sp++; *spp = sp; return 1; }
    case EA_OP_I32X4_SPLAT: { uint32_t v = S[--sp].i32; for (int i = 0; i < 4; i++) D.w[i] = v; S[sp] = (WVal){0}; memcpy(S[sp].v128, &D, 16); sp++; *spp = sp; return 1; }
    case EA_OP_I64X2_SPLAT: { uint64_t v = S[--sp].i64; D.d[0] = D.d[1] = v; S[sp] = (WVal){0}; memcpy(S[sp].v128, &D, 16); sp++; *spp = sp; return 1; }
    case EA_OP_F32X4_SPLAT: { float v = S[--sp].f32; for (int i = 0; i < 4; i++) D.f[i] = v; S[sp] = (WVal){0}; memcpy(S[sp].v128, &D, 16); sp++; *spp = sp; return 1; }
    case EA_OP_F64X2_SPLAT: { double v = S[--sp].f64; D.g[0] = D.g[1] = v; S[sp] = (WVal){0}; memcpy(S[sp].v128, &D, 16); sp++; *spp = sp; return 1; }
    case EA_OP_I8X16_EXTRACT_LANE_S: { LOAD_A; S[sp - 1].i32 = (uint32_t)(int32_t)A.sb[in->imm.u32]; *spp = sp; return 1; }
    case EA_OP_I8X16_EXTRACT_LANE_U: { LOAD_A; S[sp - 1].i32 = A.b[in->imm.u32]; *spp = sp; return 1; }
    case EA_OP_I8X16_REPLACE_LANE: { uint8_t v = (uint8_t)S[--sp].i32; memcpy(&A, S[sp - 1].v128, 16); A.b[in->imm.u32] = v; memcpy(S[sp - 1].v128, &A, 16); *spp = sp; return 1; }
    case EA_OP_I16X8_EXTRACT_LANE_S: { LOAD_A; S[sp - 1].i32 = (uint32_t)(int32_t)A.sh[in->imm.u32]; *spp = sp; return 1; }
    case EA_OP_I16X8_EXTRACT_LANE_U: { LOAD_A; S[sp - 1].i32 = A.h[in->imm.u32]; *spp = sp; return 1; }
    case EA_OP_I16X8_REPLACE_LANE: { uint16_t v = (uint16_t)S[--sp].i32; memcpy(&A, S[sp - 1].v128, 16); A.h[in->imm.u32] = v; memcpy(S[sp - 1].v128, &A, 16); *spp = sp; return 1; }
    case EA_OP_I32X4_EXTRACT_LANE: { LOAD_A; S[sp - 1].i32 = A.w[in->imm.u32]; *spp = sp; return 1; }
    case EA_OP_I32X4_REPLACE_LANE: { uint32_t v = S[--sp].i32; memcpy(&A, S[sp - 1].v128, 16); A.w[in->imm.u32] = v; memcpy(S[sp - 1].v128, &A, 16); *spp = sp; return 1; }
    case EA_OP_I64X2_EXTRACT_LANE: { LOAD_A; S[sp - 1].i64 = A.d[in->imm.u32]; *spp = sp; return 1; }
    case EA_OP_I64X2_REPLACE_LANE: { uint64_t v = S[--sp].i64; memcpy(&A, S[sp - 1].v128, 16); A.d[in->imm.u32] = v; memcpy(S[sp - 1].v128, &A, 16); *spp = sp; return 1; }
    case EA_OP_F32X4_EXTRACT_LANE: { LOAD_A; S[sp - 1].f32 = A.f[in->imm.u32]; *spp = sp; return 1; }
    case EA_OP_F32X4_REPLACE_LANE: { float v = S[--sp].f32; memcpy(&A, S[sp - 1].v128, 16); A.f[in->imm.u32] = v; memcpy(S[sp - 1].v128, &A, 16); *spp = sp; return 1; }
    case EA_OP_F64X2_EXTRACT_LANE: { LOAD_A; S[sp - 1].f64 = A.g[in->imm.u32]; *spp = sp; return 1; }
    case EA_OP_F64X2_REPLACE_LANE: { double v = S[--sp].f64; memcpy(&A, S[sp - 1].v128, 16); A.g[in->imm.u32] = v; memcpy(S[sp - 1].v128, &A, 16); *spp = sp; return 1; }
    // ================= bitwise =================
    case EA_OP_V128_NOT: {
        LOAD_A;
        uint64_t *p = (uint64_t *)&D;
        const uint64_t *q = (const uint64_t *)&A;
        p[0] = ~q[0];
        p[1] = ~q[1];
        PUT_D;
    }
    case EA_OP_V128_AND: {
        LOAD_AB;
        uint64_t *p = (uint64_t *)&D;
        const uint64_t *x = (const uint64_t *)&A, *y = (const uint64_t *)&B;
        p[0] = x[0] & y[0];
        p[1] = x[1] & y[1];
        PUT_D_DROP;
    }
    case EA_OP_V128_ANDNOT: {
        LOAD_AB;
        uint64_t *p = (uint64_t *)&D;
        const uint64_t *x = (const uint64_t *)&A, *y = (const uint64_t *)&B;
        p[0] = x[0] & ~y[0];
        p[1] = x[1] & ~y[1];
        PUT_D_DROP;
    }
    case EA_OP_V128_OR: {
        LOAD_AB;
        uint64_t *p = (uint64_t *)&D;
        const uint64_t *x = (const uint64_t *)&A, *y = (const uint64_t *)&B;
        p[0] = x[0] | y[0];
        p[1] = x[1] | y[1];
        PUT_D_DROP;
    }
    case EA_OP_V128_XOR: {
        LOAD_AB;
        uint64_t *p = (uint64_t *)&D;
        const uint64_t *x = (const uint64_t *)&A, *y = (const uint64_t *)&B;
        p[0] = x[0] ^ y[0];
        p[1] = x[1] ^ y[1];
        PUT_D_DROP;
    }
    case EA_OP_V128_BITSELECT: case EA_OP_I8X16_RELAXED_LANESELECT:
    case EA_OP_I16X8_RELAXED_LANESELECT: case EA_OP_I32X4_RELAXED_LANESELECT:
    case EA_OP_I64X2_RELAXED_LANESELECT: {
        // stack: [a, b, mask] -> (a & mask) | (b & ~mask)
        uint8_t *a = S[sp - 3].v128, *b = S[sp - 2].v128, *c = S[sp - 1].v128;
        for (int i = 0; i < 16; i++) a[i] = (uint8_t)((a[i] & c[i]) | (b[i] & ~c[i]));
        sp -= 2;
        *spp = sp;
        return 1;
    }
    // ================= reductions =================
    case EA_OP_V128_ANY_TRUE: { LOAD_A; S[sp - 1].i32 = (A.d[0] | A.d[1]) != 0; *spp = sp; return 1; }
    case EA_OP_I8X16_ALL_TRUE: { LOAD_A; int r = 1; for (int i = 0; i < 16; i++) r &= A.b[i] != 0; S[sp - 1].i32 = r; *spp = sp; return 1; }
    case EA_OP_I16X8_ALL_TRUE: { LOAD_A; int r = 1; for (int i = 0; i < 8; i++) r &= A.h[i] != 0; S[sp - 1].i32 = r; *spp = sp; return 1; }
    case EA_OP_I32X4_ALL_TRUE: { LOAD_A; int r = 1; for (int i = 0; i < 4; i++) r &= A.w[i] != 0; S[sp - 1].i32 = r; *spp = sp; return 1; }
    case EA_OP_I64X2_ALL_TRUE: { LOAD_A; int r = 1; for (int i = 0; i < 2; i++) r &= A.d[i] != 0; S[sp - 1].i32 = r; *spp = sp; return 1; }
    case EA_OP_I8X16_BITMASK: { LOAD_A; uint16_t m = 0; for (int i = 0; i < 16; i++) m |= (uint16_t)((A.sb[i] < 0) << i); S[sp - 1].i32 = m; *spp = sp; return 1; }
    case EA_OP_I16X8_BITMASK: { LOAD_A; uint8_t m = 0; for (int i = 0; i < 8; i++) m |= (uint8_t)((A.sh[i] < 0) << i); S[sp - 1].i32 = m; *spp = sp; return 1; }
    case EA_OP_I32X4_BITMASK: { LOAD_A; uint8_t m = 0; for (int i = 0; i < 4; i++) m |= (uint8_t)((A.sw[i] < 0) << i); S[sp - 1].i32 = m; *spp = sp; return 1; }
    case EA_OP_I64X2_BITMASK: { LOAD_A; uint8_t m = 0; for (int i = 0; i < 2; i++) m |= (uint8_t)((A.sd[i] < 0) << i); S[sp - 1].i32 = m; *spp = sp; return 1; }
    // ================= i8x16 =================
    case EA_OP_I8X16_EQ: CMP_I(16, uint8_t, x == y);
    case EA_OP_I8X16_NE: CMP_I(16, uint8_t, x != y);
    case EA_OP_I8X16_LT_S: CMP_I(16, int8_t, x < y);
    case EA_OP_I8X16_LT_U: CMP_I(16, uint8_t, x < y);
    case EA_OP_I8X16_GT_S: CMP_I(16, int8_t, x > y);
    case EA_OP_I8X16_GT_U: CMP_I(16, uint8_t, x > y);
    case EA_OP_I8X16_LE_S: CMP_I(16, int8_t, x <= y);
    case EA_OP_I8X16_LE_U: CMP_I(16, uint8_t, x <= y);
    case EA_OP_I8X16_GE_S: CMP_I(16, int8_t, x >= y);
    case EA_OP_I8X16_GE_U: CMP_I(16, uint8_t, x >= y);
    case EA_OP_I8X16_ADD: BIN_I(16, uint8_t, x + y);
    case EA_OP_I8X16_SUB: BIN_I(16, uint8_t, x - y);
    case EA_OP_I8X16_ADD_SAT_S: BIN_I(16, int8_t, sat_add_s8(x, y));
    case EA_OP_I8X16_ADD_SAT_U: BIN_I(16, uint8_t, sat_add_u8(x, y));
    case EA_OP_I8X16_SUB_SAT_S: BIN_I(16, int8_t, sat_sub_s8(x, y));
    case EA_OP_I8X16_SUB_SAT_U: BIN_I(16, uint8_t, sat_sub_u8(x, y));
    case EA_OP_I8X16_MIN_S: BIN_I(16, int8_t, x < y ? x : y);
    case EA_OP_I8X16_MIN_U: BIN_I(16, uint8_t, x < y ? x : y);
    case EA_OP_I8X16_MAX_S: BIN_I(16, int8_t, x > y ? x : y);
    case EA_OP_I8X16_MAX_U: BIN_I(16, uint8_t, x > y ? x : y);
    case EA_OP_I8X16_AVGR_U: BIN_I(16, uint8_t, avgr_u8(x, y));
    case EA_OP_I8X16_ABS: UN_I(16, int8_t, x < 0 ? -x : x);
    case EA_OP_I8X16_NEG: UN_I(16, int8_t, (int8_t)(0 - x));
    case EA_OP_I8X16_POPCNT: UN_I(16, uint8_t, __builtin_popcount(x));
    case EA_OP_I8X16_SHL: SHIFT_I(16, uint8_t, (uint8_t)(x << (cnt & 7)));
    case EA_OP_I8X16_SHR_S: SHIFT_I(16, int8_t, (int8_t)(x >> (cnt & 7)));
    case EA_OP_I8X16_SHR_U: SHIFT_I(16, uint8_t, (uint8_t)(x >> (cnt & 7)));
    // ================= i16x8 =================
    case EA_OP_I16X8_EQ: CMP_I(8, uint16_t, x == y);
    case EA_OP_I16X8_NE: CMP_I(8, uint16_t, x != y);
    case EA_OP_I16X8_LT_S: CMP_I(8, int16_t, x < y);
    case EA_OP_I16X8_LT_U: CMP_I(8, uint16_t, x < y);
    case EA_OP_I16X8_GT_S: CMP_I(8, int16_t, x > y);
    case EA_OP_I16X8_GT_U: CMP_I(8, uint16_t, x > y);
    case EA_OP_I16X8_LE_S: CMP_I(8, int16_t, x <= y);
    case EA_OP_I16X8_LE_U: CMP_I(8, uint16_t, x <= y);
    case EA_OP_I16X8_GE_S: CMP_I(8, int16_t, x >= y);
    case EA_OP_I16X8_GE_U: CMP_I(8, uint16_t, x >= y);
    case EA_OP_I16X8_ADD: BIN_I(8, uint16_t, x + y);
    case EA_OP_I16X8_SUB: BIN_I(8, uint16_t, x - y);
    case EA_OP_I16X8_MUL: BIN_I(8, uint16_t, x * y);
    case EA_OP_I16X8_ADD_SAT_S: BIN_I(8, int16_t, sat_add_s16(x, y));
    case EA_OP_I16X8_ADD_SAT_U: BIN_I(8, uint16_t, sat_add_u16(x, y));
    case EA_OP_I16X8_SUB_SAT_S: BIN_I(8, int16_t, sat_sub_s16(x, y));
    case EA_OP_I16X8_SUB_SAT_U: BIN_I(8, uint16_t, sat_sub_u16(x, y));
    case EA_OP_I16X8_MIN_S: BIN_I(8, int16_t, x < y ? x : y);
    case EA_OP_I16X8_MIN_U: BIN_I(8, uint16_t, x < y ? x : y);
    case EA_OP_I16X8_MAX_S: BIN_I(8, int16_t, x > y ? x : y);
    case EA_OP_I16X8_MAX_U: BIN_I(8, uint16_t, x > y ? x : y);
    case EA_OP_I16X8_AVGR_U: BIN_I(8, uint16_t, avgr_u16(x, y));
    case EA_OP_I16X8_ABS: UN_I(8, int16_t, x < 0 ? -x : x);
    case EA_OP_I16X8_NEG: UN_I(8, int16_t, (int16_t)(0 - x));
    case EA_OP_I16X8_Q15MULR_SAT_S: case EA_OP_I16X8_RELAXED_Q15MULR_S: {
        LOAD_AB;
        for (int i = 0; i < 8; i++) {
            int32_t r = ((int32_t)A.sh[i] * B.sh[i] + 0x4000) >> 15;
            D.sh[i] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
        }
        PUT_D_DROP;
    }
    case EA_OP_I16X8_SHL: SHIFT_I(8, uint16_t, (uint16_t)(x << (cnt & 15)));
    case EA_OP_I16X8_SHR_S: SHIFT_I(8, int16_t, (int16_t)(x >> (cnt & 15)));
    case EA_OP_I16X8_SHR_U: SHIFT_I(8, uint16_t, (uint16_t)(x >> (cnt & 15)));
    case EA_OP_I16X8_EXTMUL_LOW_I8X16_S: {
        LOAD_AB;
        for (int i = 0; i < 8; i++) D.sh[i] = (int16_t)(A.sb[i] * B.sb[i]);
        PUT_D_DROP;
    }
    case EA_OP_I16X8_EXTMUL_HIGH_I8X16_S: {
        LOAD_AB;
        for (int i = 0; i < 8; i++) D.sh[i] = (int16_t)(A.sb[8 + i] * B.sb[8 + i]);
        PUT_D_DROP;
    }
    case EA_OP_I16X8_EXTMUL_LOW_I8X16_U: {
        LOAD_AB;
        for (int i = 0; i < 8; i++) D.h[i] = (uint16_t)(A.b[i] * B.b[i]);
        PUT_D_DROP;
    }
    case EA_OP_I16X8_EXTMUL_HIGH_I8X16_U: {
        LOAD_AB;
        for (int i = 0; i < 8; i++) D.h[i] = (uint16_t)(A.b[8 + i] * B.b[8 + i]);
        PUT_D_DROP;
    }
    case EA_OP_I16X8_EXTADD_PAIRWISE_I8X16_S: {
        LOAD_A;
        for (int i = 0; i < 8; i++) D.sh[i] = (int16_t)((int16_t)A.sb[2 * i] + A.sb[2 * i + 1]);
        PUT_D;
    }
    case EA_OP_I16X8_EXTADD_PAIRWISE_I8X16_U: {
        LOAD_A;
        for (int i = 0; i < 8; i++) D.h[i] = (uint16_t)(A.b[2 * i] + A.b[2 * i + 1]);
        PUT_D;
    }
    // narrow
    case EA_OP_I8X16_NARROW_I16X8_S: {
        LOAD_AB;
        for (int i = 0; i < 8; i++) {
            int32_t v = A.sh[i];
            D.sb[i] = (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
        }
        for (int i = 0; i < 8; i++) {
            int32_t v = B.sh[i];
            D.sb[8 + i] = (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
        }
        PUT_D_DROP;
    }
    case EA_OP_I8X16_NARROW_I16X8_U: {
        LOAD_AB;
        for (int i = 0; i < 8; i++) {
            int32_t v = A.sh[i];
            D.b[i] = (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
        }
        for (int i = 0; i < 8; i++) {
            int32_t v = B.sh[i];
            D.b[8 + i] = (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
        }
        PUT_D_DROP;
    }
    case EA_OP_I16X8_NARROW_I32X4_S: {
        LOAD_AB;
        for (int i = 0; i < 4; i++) {
            int32_t v = A.sw[i];
            D.sh[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        for (int i = 0; i < 4; i++) {
            int32_t v = B.sw[i];
            D.sh[4 + i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        PUT_D_DROP;
    }
    case EA_OP_I16X8_NARROW_I32X4_U: {
        LOAD_AB;
        for (int i = 0; i < 4; i++) {
            int32_t v = A.sw[i];
            D.h[i] = (uint16_t)(v > 65535 ? 65535 : (v < 0 ? 0 : v));
        }
        for (int i = 0; i < 4; i++) {
            int32_t v = B.sw[i];
            D.h[4 + i] = (uint16_t)(v > 65535 ? 65535 : (v < 0 ? 0 : v));
        }
        PUT_D_DROP;
    }
    // extends
    case EA_OP_I16X8_EXTEND_LOW_I8X16_S: { LOAD_A; for (int i = 0; i < 8; i++) D.sh[i] = A.sb[i]; PUT_D; }
    case EA_OP_I16X8_EXTEND_HIGH_I8X16_S: { LOAD_A; for (int i = 0; i < 8; i++) D.sh[i] = A.sb[8 + i]; PUT_D; }
    case EA_OP_I16X8_EXTEND_LOW_I8X16_U: { LOAD_A; for (int i = 0; i < 8; i++) D.h[i] = A.b[i]; PUT_D; }
    case EA_OP_I16X8_EXTEND_HIGH_I8X16_U: { LOAD_A; for (int i = 0; i < 8; i++) D.h[i] = A.b[8 + i]; PUT_D; }
    // ================= i32x4 =================
    case EA_OP_I32X4_EQ: CMP_I(4, uint32_t, x == y);
    case EA_OP_I32X4_NE: CMP_I(4, uint32_t, x != y);
    case EA_OP_I32X4_LT_S: CMP_I(4, int32_t, x < y);
    case EA_OP_I32X4_LT_U: CMP_I(4, uint32_t, x < y);
    case EA_OP_I32X4_GT_S: CMP_I(4, int32_t, x > y);
    case EA_OP_I32X4_GT_U: CMP_I(4, uint32_t, x > y);
    case EA_OP_I32X4_LE_S: CMP_I(4, int32_t, x <= y);
    case EA_OP_I32X4_LE_U: CMP_I(4, uint32_t, x <= y);
    case EA_OP_I32X4_GE_S: CMP_I(4, int32_t, x >= y);
    case EA_OP_I32X4_GE_U: CMP_I(4, uint32_t, x >= y);
    case EA_OP_I32X4_ADD: BIN_I(4, uint32_t, x + y);
    case EA_OP_I32X4_SUB: BIN_I(4, uint32_t, x - y);
    case EA_OP_I32X4_MUL: BIN_I(4, uint32_t, x * y);
    case EA_OP_I32X4_MIN_S: BIN_I(4, int32_t, x < y ? x : y);
    case EA_OP_I32X4_MIN_U: BIN_I(4, uint32_t, x < y ? x : y);
    case EA_OP_I32X4_MAX_S: BIN_I(4, int32_t, x > y ? x : y);
    case EA_OP_I32X4_MAX_U: BIN_I(4, uint32_t, x > y ? x : y);
    case EA_OP_I32X4_ABS: UN_I(4, int32_t, x < 0 ? -x : x);
    case EA_OP_I32X4_NEG: UN_I(4, int32_t, (int32_t)(0 - (int64_t)x));
    case EA_OP_I32X4_SHL: SHIFT_I(4, uint32_t, x << (cnt & 31));
    case EA_OP_I32X4_SHR_S: SHIFT_I(4, int32_t, x >> (cnt & 31));
    case EA_OP_I32X4_SHR_U: SHIFT_I(4, uint32_t, x >> (cnt & 31));
    case EA_OP_I32X4_DOT_I16X8_S: {
        LOAD_AB;
        for (int i = 0; i < 4; i++)
            D.sw[i] = (int32_t)A.sh[2 * i] * B.sh[2 * i] + (int32_t)A.sh[2 * i + 1] * B.sh[2 * i + 1];
        PUT_D_DROP;
    }
    case EA_OP_I32X4_EXTMUL_LOW_I16X8_S: {
        LOAD_AB;
        for (int i = 0; i < 4; i++) D.sw[i] = (int32_t)A.sh[i] * (int32_t)B.sh[i];
        PUT_D_DROP;
    }
    case EA_OP_I32X4_EXTMUL_HIGH_I16X8_S: {
        LOAD_AB;
        for (int i = 0; i < 4; i++) D.sw[i] = (int32_t)A.sh[4 + i] * (int32_t)B.sh[4 + i];
        PUT_D_DROP;
    }
    case EA_OP_I32X4_EXTMUL_LOW_I16X8_U: {
        LOAD_AB;
        for (int i = 0; i < 4; i++) D.w[i] = (uint32_t)A.h[i] * (uint32_t)B.h[i];
        PUT_D_DROP;
    }
    case EA_OP_I32X4_EXTMUL_HIGH_I16X8_U: {
        LOAD_AB;
        for (int i = 0; i < 4; i++) D.w[i] = (uint32_t)A.h[4 + i] * (uint32_t)B.h[4 + i];
        PUT_D_DROP;
    }
    case EA_OP_I32X4_EXTADD_PAIRWISE_I16X8_S: {
        LOAD_A;
        for (int i = 0; i < 4; i++) D.sw[i] = (int32_t)A.sh[2 * i] + A.sh[2 * i + 1];
        PUT_D;
    }
    case EA_OP_I32X4_EXTADD_PAIRWISE_I16X8_U: {
        LOAD_A;
        for (int i = 0; i < 4; i++) D.w[i] = (uint32_t)A.h[2 * i] + A.h[2 * i + 1];
        PUT_D;
    }
    case EA_OP_I32X4_EXTEND_LOW_I16X8_S: { LOAD_A; for (int i = 0; i < 4; i++) D.sw[i] = A.sh[i]; PUT_D; }
    case EA_OP_I32X4_EXTEND_HIGH_I16X8_S: { LOAD_A; for (int i = 0; i < 4; i++) D.sw[i] = A.sh[4 + i]; PUT_D; }
    case EA_OP_I32X4_EXTEND_LOW_I16X8_U: { LOAD_A; for (int i = 0; i < 4; i++) D.w[i] = A.h[i]; PUT_D; }
    case EA_OP_I32X4_EXTEND_HIGH_I16X8_U: { LOAD_A; for (int i = 0; i < 4; i++) D.w[i] = A.h[4 + i]; PUT_D; }
    // ================= i64x2 =================
    case EA_OP_I64X2_EQ: CMP_I(2, uint64_t, x == y);
    case EA_OP_I64X2_NE: CMP_I(2, uint64_t, x != y);
    case EA_OP_I64X2_LT_S: CMP_I(2, int64_t, x < y);
    case EA_OP_I64X2_GT_S: CMP_I(2, int64_t, x > y);
    case EA_OP_I64X2_LE_S: CMP_I(2, int64_t, x <= y);
    case EA_OP_I64X2_GE_S: CMP_I(2, int64_t, x >= y);
    case EA_OP_I64X2_ADD: BIN_I(2, uint64_t, x + y);
    case EA_OP_I64X2_SUB: BIN_I(2, uint64_t, x - y);
    case EA_OP_I64X2_MUL: BIN_I(2, uint64_t, x * y);
    case EA_OP_I64X2_ABS: UN_I(2, int64_t, x < 0 ? -x : x);
    case EA_OP_I64X2_NEG: UN_I(2, int64_t, (int64_t)(0 - x));
    case EA_OP_I64X2_SHL: SHIFT_I(2, uint64_t, x << (cnt & 63));
    case EA_OP_I64X2_SHR_S: SHIFT_I(2, int64_t, x >> (cnt & 63));
    case EA_OP_I64X2_SHR_U: SHIFT_I(2, uint64_t, x >> (cnt & 63));
    case EA_OP_I64X2_EXTMUL_LOW_I32X4_S: {
        LOAD_AB;
        for (int i = 0; i < 2; i++) D.sd[i] = (int64_t)A.sw[i] * (int64_t)B.sw[i];
        PUT_D_DROP;
    }
    case EA_OP_I64X2_EXTMUL_HIGH_I32X4_S: {
        LOAD_AB;
        for (int i = 0; i < 2; i++) D.sd[i] = (int64_t)A.sw[2 + i] * (int64_t)B.sw[2 + i];
        PUT_D_DROP;
    }
    case EA_OP_I64X2_EXTMUL_LOW_I32X4_U: {
        LOAD_AB;
        for (int i = 0; i < 2; i++) D.sd[i] = (int64_t)((uint64_t)A.w[i] * B.w[i]);
        PUT_D_DROP;
    }
    case EA_OP_I64X2_EXTMUL_HIGH_I32X4_U: {
        LOAD_AB;
        for (int i = 0; i < 2; i++) D.sd[i] = (int64_t)((uint64_t)A.w[2 + i] * B.w[2 + i]);
        PUT_D_DROP;
    }
    case EA_OP_I64X2_EXTEND_LOW_I32X4_S: { LOAD_A; for (int i = 0; i < 2; i++) D.sd[i] = A.sw[i]; PUT_D; }
    case EA_OP_I64X2_EXTEND_HIGH_I32X4_S: { LOAD_A; for (int i = 0; i < 2; i++) D.sd[i] = A.sw[2 + i]; PUT_D; }
    case EA_OP_I64X2_EXTEND_LOW_I32X4_U: { LOAD_A; for (int i = 0; i < 2; i++) D.d[i] = A.w[i]; PUT_D; }
    case EA_OP_I64X2_EXTEND_HIGH_I32X4_U: { LOAD_A; for (int i = 0; i < 2; i++) D.d[i] = A.w[2 + i]; PUT_D; }
    // ================= f32x4 =================
    case EA_OP_F32X4_EQ: CMP_I(4, uint32_t, (*(float *)&x) == (*(float *)&y));
    case EA_OP_F32X4_NE: CMP_I(4, uint32_t, (*(float *)&x) != (*(float *)&y));
    case EA_OP_F32X4_LT: CMP_I(4, uint32_t, (*(float *)&x) < (*(float *)&y));
    case EA_OP_F32X4_GT: CMP_I(4, uint32_t, (*(float *)&x) > (*(float *)&y));
    case EA_OP_F32X4_LE: CMP_I(4, uint32_t, (*(float *)&x) <= (*(float *)&y));
    case EA_OP_F32X4_GE: CMP_I(4, uint32_t, (*(float *)&x) >= (*(float *)&y));
    case EA_OP_F32X4_ADD: BIN_F32(A.f[i] + B.f[i]);
    case EA_OP_F32X4_SUB: BIN_F32(A.f[i] - B.f[i]);
    case EA_OP_F32X4_MUL: BIN_F32(A.f[i] * B.f[i]);
    case EA_OP_F32X4_DIV: BIN_F32(A.f[i] / B.f[i]);
    case EA_OP_F32X4_MIN: BIN_F32(simd_fmin32(A.f[i], B.f[i]));
    case EA_OP_F32X4_MAX: BIN_F32(simd_fmax32(A.f[i], B.f[i]));
    case EA_OP_F32X4_PMIN: BIN_F32(B.f[i] < A.f[i] ? B.f[i] : A.f[i]);
    case EA_OP_F32X4_PMAX: BIN_F32(A.f[i] < B.f[i] ? B.f[i] : A.f[i]);
    case EA_OP_F32X4_ABS: { LOAD_A; for (int i = 0; i < 4; i++) D.w[i] = A.w[i] & 0x7FFFFFFFu; PUT_D; }
    case EA_OP_F32X4_NEG: { LOAD_A; for (int i = 0; i < 4; i++) D.w[i] = A.w[i] ^ 0x80000000u; PUT_D; }
    case EA_OP_F32X4_SQRT: { LOAD_A; for (int i = 0; i < 4; i++) { float v = A.f[i]; D.f[i] = v < 0 ? simd_f32_nan() : sqrtf(v); } PUT_D; }
    case EA_OP_F32X4_CEIL: { LOAD_A; for (int i = 0; i < 4; i++) D.f[i] = ceilf(A.f[i]); PUT_D; }
    case EA_OP_F32X4_FLOOR: { LOAD_A; for (int i = 0; i < 4; i++) D.f[i] = floorf(A.f[i]); PUT_D; }
    case EA_OP_F32X4_TRUNC: { LOAD_A; for (int i = 0; i < 4; i++) D.f[i] = truncf(A.f[i]); PUT_D; }
    case EA_OP_F32X4_NEAREST: { LOAD_A; for (int i = 0; i < 4; i++) D.f[i] = nearbyintf(A.f[i]); PUT_D; }
    // ================= f64x2 =================
    case EA_OP_F64X2_EQ: CMP_I(2, uint64_t, (*(double *)&x) == (*(double *)&y));
    case EA_OP_F64X2_NE: CMP_I(2, uint64_t, (*(double *)&x) != (*(double *)&y));
    case EA_OP_F64X2_LT: CMP_I(2, uint64_t, (*(double *)&x) < (*(double *)&y));
    case EA_OP_F64X2_GT: CMP_I(2, uint64_t, (*(double *)&x) > (*(double *)&y));
    case EA_OP_F64X2_LE: CMP_I(2, uint64_t, (*(double *)&x) <= (*(double *)&y));
    case EA_OP_F64X2_GE: CMP_I(2, uint64_t, (*(double *)&x) >= (*(double *)&y));
    case EA_OP_F64X2_ADD: BIN_F64(A.g[i] + B.g[i]);
    case EA_OP_F64X2_SUB: BIN_F64(A.g[i] - B.g[i]);
    case EA_OP_F64X2_MUL: BIN_F64(A.g[i] * B.g[i]);
    case EA_OP_F64X2_DIV: BIN_F64(A.g[i] / B.g[i]);
    case EA_OP_F64X2_MIN: BIN_F64(simd_fmin64(A.g[i], B.g[i]));
    case EA_OP_F64X2_MAX: BIN_F64(simd_fmax64(A.g[i], B.g[i]));
    case EA_OP_F64X2_PMIN: BIN_F64(B.g[i] < A.g[i] ? B.g[i] : A.g[i]);
    case EA_OP_F64X2_PMAX: BIN_F64(A.g[i] < B.g[i] ? B.g[i] : A.g[i]);
    case EA_OP_F64X2_ABS: { LOAD_A; for (int i = 0; i < 2; i++) D.d[i] = A.d[i] & 0x7FFFFFFFFFFFFFFFull; PUT_D; }
    case EA_OP_F64X2_NEG: { LOAD_A; for (int i = 0; i < 2; i++) D.d[i] = A.d[i] ^ 0x8000000000000000ull; PUT_D; }
    case EA_OP_F64X2_SQRT: { LOAD_A; for (int i = 0; i < 2; i++) { double v = A.g[i]; D.g[i] = v < 0 ? simd_f64_nan() : sqrt(v); } PUT_D; }
    case EA_OP_F64X2_CEIL: { LOAD_A; for (int i = 0; i < 2; i++) D.g[i] = ceil(A.g[i]); PUT_D; }
    case EA_OP_F64X2_FLOOR: { LOAD_A; for (int i = 0; i < 2; i++) D.g[i] = floor(A.g[i]); PUT_D; }
    case EA_OP_F64X2_TRUNC: { LOAD_A; for (int i = 0; i < 2; i++) D.g[i] = trunc(A.g[i]); PUT_D; }
    case EA_OP_F64X2_NEAREST: { LOAD_A; for (int i = 0; i < 2; i++) D.g[i] = nearbyint(A.g[i]); PUT_D; }
    // ================= conversions =================
    case EA_OP_I32X4_TRUNC_SAT_F32X4_S: {
        LOAD_A;
        for (int i = 0; i < 4; i++) {
            float v = A.f[i];
            D.sw[i] = v != v ? 0 : (v >= 2147483648.0f ? INT32_MAX : (v < -2147483648.0f ? INT32_MIN : (int32_t)v));
        }
        PUT_D;
    }
    case EA_OP_I32X4_TRUNC_SAT_F32X4_U: {
        LOAD_A;
        for (int i = 0; i < 4; i++) {
            float v = A.f[i];
            D.w[i] = v != v ? 0 : (v >= 4294967296.0f ? 0xFFFFFFFFu : (v <= -1.0f ? 0 : (uint32_t)v));
        }
        PUT_D;
    }
    case EA_OP_F32X4_CONVERT_I32X4_S: { LOAD_A; for (int i = 0; i < 4; i++) D.f[i] = (float)A.sw[i]; PUT_D; }
    case EA_OP_F32X4_CONVERT_I32X4_U: { LOAD_A; for (int i = 0; i < 4; i++) D.f[i] = (float)A.w[i]; PUT_D; }
    case EA_OP_I32X4_TRUNC_SAT_F64X2_S_ZERO: {
        LOAD_A;
        for (int i = 0; i < 2; i++) {
            double v = A.g[i];
            D.sw[i] = v != v ? 0 : (v >= 2147483648.0 ? INT32_MAX : (v < -2147483648.0 ? INT32_MIN : (int32_t)v));
        }
        D.sw[2] = D.sw[3] = 0;
        PUT_D;
    }
    case EA_OP_I32X4_TRUNC_SAT_F64X2_U_ZERO: {
        LOAD_A;
        for (int i = 0; i < 2; i++) {
            double v = A.g[i];
            D.w[i] = v != v ? 0 : (v >= 4294967296.0 ? 0xFFFFFFFFu : (v <= -1.0 ? 0 : (uint32_t)v));
        }
        D.w[2] = D.w[3] = 0;
        PUT_D;
    }
    case EA_OP_F64X2_CONVERT_LOW_I32X4_S: { LOAD_A; for (int i = 0; i < 2; i++) D.g[i] = (double)A.sw[i]; PUT_D; }
    case EA_OP_F64X2_CONVERT_LOW_I32X4_U: { LOAD_A; for (int i = 0; i < 2; i++) D.g[i] = (double)A.w[i]; PUT_D; }
    case EA_OP_F32X4_DEMOTE_F64X2_ZERO: {
        LOAD_A;
        for (int i = 0; i < 2; i++) D.f[i] = (float)A.g[i];
        D.f[2] = D.f[3] = 0.0f;
        PUT_D;
    }
    case EA_OP_F64X2_PROMOTE_LOW_F32X4: { LOAD_A; for (int i = 0; i < 2; i++) D.g[i] = (double)A.f[i]; PUT_D; }
    // ================= relaxed =================
    case EA_OP_F32X4_RELAXED_MADD: { LOAD_ABC; for (int i = 0; i < 4; i++) D.f[i] = A.f[i] * B.f[i] + C.f[i]; PUT_D_DROP2; }
    case EA_OP_F32X4_RELAXED_NMADD: { LOAD_ABC; for (int i = 0; i < 4; i++) D.f[i] = -(A.f[i] * B.f[i]) + C.f[i]; PUT_D_DROP2; }
    case EA_OP_F64X2_RELAXED_MADD: { LOAD_ABC; for (int i = 0; i < 2; i++) D.g[i] = A.g[i] * B.g[i] + C.g[i]; PUT_D_DROP2; }
    case EA_OP_F64X2_RELAXED_NMADD: { LOAD_ABC; for (int i = 0; i < 2; i++) D.g[i] = -(A.g[i] * B.g[i]) + C.g[i]; PUT_D_DROP2; }
    case EA_OP_F32X4_RELAXED_MIN: BIN_F32(simd_fmin32(A.f[i], B.f[i]));
    case EA_OP_F32X4_RELAXED_MAX: BIN_F32(simd_fmax32(A.f[i], B.f[i]));
    case EA_OP_F64X2_RELAXED_MIN: BIN_F64(simd_fmin64(A.g[i], B.g[i]));
    case EA_OP_F64X2_RELAXED_MAX: BIN_F64(simd_fmax64(A.g[i], B.g[i]));
    case EA_OP_I32X4_RELAXED_TRUNC_F64X2_S_ZERO: {
        LOAD_A;
        for (int i = 0; i < 2; i++) {
            double v = A.g[i];
            D.sw[i] = v != v ? 0 : (v >= 2147483648.0 ? INT32_MAX : (v < -2147483648.0 ? INT32_MIN : (int32_t)v));
        }
        D.sw[2] = D.sw[3] = 0;
        PUT_D;
    }
    case EA_OP_I32X4_RELAXED_TRUNC_F64X2_U_ZERO: {
        LOAD_A;
        for (int i = 0; i < 2; i++) {
            double v = A.g[i];
            D.w[i] = v != v ? 0 : (v >= 4294967296.0 ? 0xFFFFFFFFu : (v <= -1.0 ? 0 : (uint32_t)v));
        }
        D.w[2] = D.w[3] = 0;
        PUT_D;
    }
    case EA_OP_I8X16_RELAXED_SWIZZLE: {
        LOAD_AB;
        for (int i = 0; i < 16; i++) {
            uint8_t k = B.b[i];
            D.b[i] = k < 16 ? A.b[k] : 0;
        }
        PUT_D_DROP;
    }
    case EA_OP_I16X8_RELAXED_DOT_I8X16_I7X16_S: {
        LOAD_AB;
        for (int i = 0; i < 8; i++) {
            int32_t sum = (int32_t)A.sb[2 * i] * B.sb[2 * i] + (int32_t)A.sb[2 * i + 1] * B.sb[2 * i + 1];
            D.sh[i] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
        }
        PUT_D_DROP;
    }
    case EA_OP_I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S: {
        LOAD_ABC;
        for (int i = 0; i < 4; i++)
            D.sw[i] = (int32_t)A.sb[4 * i] * B.sb[4 * i] + (int32_t)A.sb[4 * i + 1] * B.sb[4 * i + 1] +
                      (int32_t)A.sb[4 * i + 2] * B.sb[4 * i + 2] + (int32_t)A.sb[4 * i + 3] * B.sb[4 * i + 3] +
                      C.sw[i];
        PUT_D_DROP2;
    }
    case EA_OP_I32X4_RELAXED_TRUNC_F32X4_S: { LOAD_A; for (int i = 0; i < 4; i++) D.sw[i] = (int32_t)A.f[i]; PUT_D; }
    case EA_OP_I32X4_RELAXED_TRUNC_F32X4_U: { LOAD_A; for (int i = 0; i < 4; i++) D.w[i] = (uint32_t)A.f[i]; PUT_D; }
    default:
        return 0;
    }
}
