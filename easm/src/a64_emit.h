// Minimal AArch64 machine-code emitter for the easm baseline JIT.
// Each emit_* writes one 4-byte instruction into the buffer.
// Branch fixups are resolved relative to section start at finish time.
#ifndef A64_EMIT_H
#define A64_EMIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

// A64 register numbers
#define R0 0
#define R1 1
#define R2 2
#define R3 3
#define R4 4
#define R5 5
#define R6 6
#define R7 7
#define R8 8
#define R9 9
#define R10 10
#define R11 11
#define R12 12
#define R13 13
#define R14 14
#define R15 15
#define R16 16 // intra-call scratch (IP0)
#define R17 17 // intra-call scratch (IP1)
#define R18 18 // platform (reserved)
#define R19 19
#define R20 20
#define R21 21
#define R22 22
#define R23 23
#define R24 24
#define R25 25 // easm JIT: memory base
#define R26 26 // easm JIT: memory limit (base + size)
#define R27 27 // easm JIT: EaExec*
#define R28 28 // easm JIT: EaInstance*
#define FP 29
#define LR 30
#define SP 31
#define XZR 31

// condition codes
#define CC_EQ 0
#define CC_NE 1
#define CC_CS 2
#define CC_HS 2
#define CC_CC 3
#define CC_LO 3
#define CC_MI 4
#define CC_PL 5
#define CC_VS 6
#define CC_VC 7
#define CC_HI 8
#define CC_LS 9
#define CC_GE 10
#define CC_LT 11
#define CC_GT 12
#define CC_LE 13

typedef struct {
    uint32_t *buf;
    size_t len;    // instructions emitted
    size_t cap;
    // branch fixups: patch instruction at `at` to reach `target` (instruction index)
    struct { uint32_t at, target; bool is_cond; } *fix;
    size_t nfix, capfix;
} Em;

static void em_init(Em *e) { memset(e, 0, sizeof(*e)); }
static void em_free(Em *e) { free(e->buf); free(e->fix); }

static inline void em_word(Em *e, uint32_t w) {
    if (e->len == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 256;
        e->buf = (uint32_t *)realloc(e->buf, e->cap * 4);
    }
    e->buf[e->len++] = w;
}

// branch placeholder targeting instruction index `target`
static void em_b_label(Em *e, uint32_t target) {
    em_word(e, 0x14000000);
    if (e->nfix == e->capfix) {
        e->capfix = e->capfix ? e->capfix * 2 : 16;
        e->fix = realloc(e->fix, e->capfix * sizeof(*e->fix));
    }
    e->fix[e->nfix].at = e->len - 1;
    e->fix[e->nfix].target = target;
    e->fix[e->nfix].is_cond = false;
    e->nfix++;
}
static void em_bcond_label(Em *e, uint32_t target, uint32_t cond) {
    em_word(e, 0x54000000 | cond);
    if (e->nfix == e->capfix) {
        e->capfix = e->capfix ? e->capfix * 2 : 16;
        e->fix = realloc(e->fix, e->capfix * sizeof(*e->fix));
    }
    e->fix[e->nfix].at = e->len - 1;
    e->fix[e->nfix].target = target;
    e->fix[e->nfix].is_cond = true;
    e->nfix++;
}
static inline void em_resolve(Em *e) {
    for (size_t i = 0; i < e->nfix; i++) {
        int64_t off = (int64_t)e->fix[i].target - (int64_t)e->fix[i].at;
        if (e->fix[i].is_cond) {
            if (off < -(1 << 18) || off > (1 << 18)) abort();
            e->buf[e->fix[i].at] |= ((uint32_t)off & 0x7FFFF) << 5;
        } else {
            if (off < -(1 << 24) || off > (1 << 24)) abort();
            e->buf[e->fix[i].at] |= ((uint32_t)off) & 0x3FFFFFF;
        }
    }
    e->nfix = 0;
}

// ---- data processing
static inline void a64_add_imm64(Em *e, uint32_t rd, uint32_t rn, uint32_t imm12) {
    em_word(e, 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static inline void a64_add_imm32(Em *e, uint32_t rd, uint32_t rn, uint32_t imm12) {
    em_word(e, 0x11000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static inline void a64_sub_imm64(Em *e, uint32_t rd, uint32_t rn, uint32_t imm12) {
    em_word(e, 0xD1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static inline void a64_sub_imm32(Em *e, uint32_t rd, uint32_t rn, uint32_t imm12) {
    em_word(e, 0x51000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static inline void a64_add_reg64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x8B000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_sub_reg64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0xCB000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_add_reg32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x0B000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_sub_reg32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x4B000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_mov_reg64(Em *e, uint32_t rd, uint32_t rm) { // orr rd, xzr, rm
    em_word(e, 0xAA000000 | (rm << 16) | (31 << 5) | rd);
}
static inline void a64_mov_reg32(Em *e, uint32_t rd, uint32_t rm) {
    em_word(e, 0x2A000000 | (rm << 16) | (31 << 5) | rd);
}
static inline void a64_movz64(Em *e, uint32_t rd, uint32_t imm16, uint32_t hw) {
    em_word(e, 0xD2800000 | (hw << 21) | ((imm16 & 0xFFFF) << 5) | rd);
}
static inline void a64_movk64(Em *e, uint32_t rd, uint32_t imm16, uint32_t hw) {
    em_word(e, 0xF2800000 | (hw << 21) | ((imm16 & 0xFFFF) << 5) | rd);
}
static inline void a64_movz32(Em *e, uint32_t rd, uint32_t imm16) {
    em_word(e, 0x52800000 | ((imm16 & 0xFFFF) << 5) | rd);
}
static inline void a64_mov64_imm(Em *e, uint32_t rd, uint64_t v) { // synthetic (1-4 insns)
    if (v == 0) { a64_mov_reg64(e, rd, 31); return; }
    int last = 63;
    while (last >= 0 && !((v >> last) & 1)) last--;
    int hw = last / 16; // highest significant halfword
    bool first = true;
    for (int h = 0; h <= hw; h++) {
        uint32_t bits = (uint32_t)((v >> (16 * h)) & 0xFFFF);
        if (bits == 0 && first) { if (h == hw) { a64_movz64(e, rd, 0, 0); return; } continue; }
        if (first) { a64_movz64(e, rd, bits, h); first = false; }
        else a64_movk64(e, rd, bits, h);
    }
}
// correct 32-bit movk (w register): 0x72A00000
static inline void a64_movk32(Em *e, uint32_t rd, uint32_t imm16) {
    em_word(e, 0x72A00000 | ((imm16 & 0xFFFF) << 5) | rd);
}
static inline void a64_mov32_imm(Em *e, uint32_t rd, uint32_t v) {
    if ((v & 0xFFFF0000) == 0) { a64_movz32(e, rd, v & 0xFFFF); return; }
    a64_movz32(e, rd, v & 0xFFFF);
    a64_movk32(e, rd, v >> 16);
}

// ---- loads/stores
static inline void a64_ldr_imm64(Em *e, uint32_t rt, uint32_t rn, int64_t off) { // off %8==0
    if (off >= 0 && off < 32760 && (off & 7) == 0)
        em_word(e, 0xF9400000 | ((uint32_t)(off >> 3) << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0xF8400000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_ldr_imm64(e, rt, t, 0);
    }
}
static inline void a64_str_imm64(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 32760 && (off & 7) == 0)
        em_word(e, 0xF9000000 | ((uint32_t)(off >> 3) << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0xF8000000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_str_imm64(e, rt, t, 0);
    }
}
static inline void a64_ldr_imm32(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 16380 && (off & 3) == 0)
        em_word(e, 0xB9400000 | ((uint32_t)(off >> 2) << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0xB8400000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_ldr_imm32(e, rt, t, 0);
    }
}
static inline void a64_str_imm32(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 16380 && (off & 3) == 0)
        em_word(e, 0xB9000000 | ((uint32_t)(off >> 2) << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0xB8000000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_str_imm32(e, rt, t, 0);
    }
}
static inline void a64_ldrb_imm32(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 4095)
        em_word(e, 0x39400000 | ((uint32_t)off << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0x38400000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_ldrb_imm32(e, rt, t, 0);
    }
}
static inline void a64_ldrsb_imm32(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 4095)
        em_word(e, 0x39800000 | ((uint32_t)off << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0x38800000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_ldrsb_imm32(e, rt, t, 0);
    }
}
static inline void a64_ldrsh_imm32(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 4095)
        em_word(e, 0x79800000 | ((uint32_t)off << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0x78800000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_ldrsh_imm32(e, rt, t, 0);
    }
}
static inline void a64_ldrh_imm32(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 8190 && (off & 1) == 0)
        em_word(e, 0x79400000 | ((uint32_t)(off >> 1) << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0x78400000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_ldrh_imm32(e, rt, t, 0);
    }
}
static inline void a64_strb_imm32(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 4095)
        em_word(e, 0x39000000 | ((uint32_t)off << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0x38000000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_strb_imm32(e, rt, t, 0);
    }
}
static inline void a64_strh_imm32(Em *e, uint32_t rt, uint32_t rn, int64_t off) {
    if (off >= 0 && off < 8190 && (off & 1) == 0)
        em_word(e, 0x79000000 | ((uint32_t)(off >> 1) << 10) | (rn << 5) | rt);
    else if (off >= -256 && off <= 255)
        em_word(e, 0x78000000 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | rt);
    else { // out of imm9 range: materialize the address
        uint32_t t = (rn == 17) ? 16 : 17;
        a64_mov64_imm(e, t, (uint64_t)off);
        a64_add_reg64(e, t, t, rn);
        a64_strh_imm32(e, rt, t, 0);
    }
}
// pre-index push: str Xt, [sp, #-16]!  (64-bit store, pre-index)

static inline void a64_str_pre64(Em *e, uint32_t rt, uint32_t rn, int32_t imm9) {
    em_word(e, 0xF8000C00 | ((uint32_t)(imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static inline void a64_ldr_post64(Em *e, uint32_t rt, uint32_t rn, int32_t imm9) {
    em_word(e, 0xF8400400 | ((uint32_t)(imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static inline void a64_ldr_pre64(Em *e, uint32_t rt, uint32_t rn, int32_t imm9) {
    em_word(e, 0xF8000C00 | ((uint32_t)(imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static inline void a64_str_pre32(Em *e, uint32_t rt, uint32_t rn, int32_t imm9) {
    em_word(e, 0xB8000C00 | ((uint32_t)(imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static inline void a64_ldr_post32(Em *e, uint32_t rt, uint32_t rn, int32_t imm9) {
    em_word(e, 0xB8400400 | ((uint32_t)(imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
// LDR/STR register offset: [Xn, Xm]
static inline void a64_ldr_reg64(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0xF8606800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_str_reg64(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0xF8206800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_ldr_reg32(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0xB8606800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_str_reg32(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0xB8206800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_ldrb_reg32(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0x38606800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_ldrsb_reg32(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0x38E06800 | (rm << 16) | (rn << 5) | rt); // 32-bit dest signed byte
}
static inline void a64_ldrsb_reg64(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0x38A06800 | (rm << 16) | (rn << 5) | rt); // 64-bit dest signed byte
}
static inline void a64_ldrh_reg32(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0x78606800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_ldrsh_reg32(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0x78E06800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_ldrsh_reg64(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0x78A06800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_strb_reg32(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0x38206800 | (rm << 16) | (rn << 5) | rt);
}
static inline void a64_strh_reg32(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) {
    em_word(e, 0x78206800 | (rm << 16) | (rn << 5) | rt);
}
// stp / ldp 64-bit
// imm7 in BYTES (scaled by 8 internally); must be multiple of 8
// stp with signed 7-bit scaled offset (unsigned-offset form, no writeback)
static inline void a64_stp_off64(Em *e, uint32_t rt1, uint32_t rt2, uint32_t rn, int32_t imm7) {
    em_word(e, 0xA9000000u | ((uint32_t)(imm7 & 0x7F) << 15) | (rn << 5) | (rt1) | (rt2 << 10));
}
// ldp with signed 7-bit scaled offset (imm7 in 8-byte units for 64-bit)
static inline void a64_ldp_off64(Em *e, uint32_t rt1, uint32_t rt2, uint32_t rn, int32_t imm7) {
    em_word(e, 0xA9400000u | ((uint32_t)(imm7 & 0x7F) << 15) | (rn << 5) | (rt1) | (rt2 << 10));
}
static inline void a64_stp_pre64(Em *e, uint32_t rt1, uint32_t rt2, uint32_t rn, int32_t imm7) {
    int32_t v = imm7 / 8;
    em_word(e, 0xA9800000 | ((uint32_t)(v & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}
static inline void a64_ldp_post64(Em *e, uint32_t rt1, uint32_t rt2, uint32_t rn, int32_t imm7) {
    int32_t v = imm7 / 8;
    em_word(e, 0xA8C00000 | ((uint32_t)(v & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}
// mov between sp and gpr: add rd, sp, #0 / add rd, fp, #0
static inline void a64_mov_from_sp(Em *e, uint32_t rd) {
    em_word(e, 0x91000000 | (31 << 5) | rd);
}
static inline void a64_mov_sp_from(Em *e, uint32_t rn) {
    em_word(e, 0x91000000 | (rn << 5) | 31);
}

// ---- compares & branches
static inline void a64_cmp_imm32(Em *e, uint32_t rn, uint32_t imm12) {
    em_word(e, 0x71000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | 31);
}
static inline void a64_cmp_imm64(Em *e, uint32_t rn, uint32_t imm12) {
    em_word(e, 0xF1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | 31);
}
static inline void a64_cmp_reg64(Em *e, uint32_t rn, uint32_t rm) {
    em_word(e, 0xEB000000 | (rm << 16) | (rn << 5) | 31);
}
static inline void a64_cmp_reg32(Em *e, uint32_t rn, uint32_t rm) {
    em_word(e, 0x6B000000 | (rm << 16) | (rn << 5) | 31);
}
static inline void a64_b(Em *e, int64_t insn_off) {
    em_word(e, 0x14000000 | ((uint32_t)insn_off & 0x3FFFFFF));
}
static inline void a64_bcond(Em *e, int64_t insn_off, uint32_t cond) {
    em_word(e, 0x54000000 | ((uint32_t)(insn_off & 0x7FFFF) << 5) | cond);
}
static inline void a64_blr(Em *e, uint32_t rn) { em_word(e, 0xD63F0000 | (rn << 5)); }
static inline void a64_br_reg(Em *e, uint32_t rn) { em_word(e, 0xD61F0000 | (rn << 5)); }
static inline void a64_br(Em *e, uint32_t rn) { em_word(e, 0xD61F0000 | (rn << 5)); }
static inline void a64_ret(Em *e) { em_word(e, 0xD65F03C0); }
static inline void a64_nop(Em *e) { em_word(e, 0xD503201F); }
// bl to absolute function pointer via literal-less sequence: movz/movk x16 + blr
static inline void a64_mov64_imm_abs(Em *e, uint32_t rd, uint64_t v) { a64_mov64_imm(e, rd, v); }

// divide
static inline void a64_sdiv32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x1AC00C00 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_udiv32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x1AC00800 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_sdiv64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x9AC00C00 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_udiv64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x9AC00800 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_msub32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra) {
    em_word(e, 0x1B008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}
static inline void a64_msub64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra) {
    em_word(e, 0x9B008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}
static inline void a64_madd32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra) {
    em_word(e, 0x1B000000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}
static inline void a64_madd64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra) {
    em_word(e, 0x9B000000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}
// bit ops
static inline void a64_and_imm32(Em *e, uint32_t rd, uint32_t rn, uint32_t bitmask_imm) {
    em_word(e, 0x12000000 | (bitmask_imm << 10) | (rn << 5) | rd);
}
static inline void a64_orr_reg32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x2A000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_and_reg32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x0A000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_and_reg64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x8A000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_orr_reg64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0xAA000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_eor_reg32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x4A000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_neg32(Em *e, uint32_t rd, uint32_t rn) {
    em_word(e, 0x4B000000 | (rn << 16) | (31 << 5) | rd); // sub rd, wzr, rn
}
static inline void a64_neg64(Em *e, uint32_t rd, uint32_t rn) {
    em_word(e, 0xCB000000 | (rn << 16) | (31 << 5) | rd);
}
static inline void a64_eor_reg64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0xCA000000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_lslv32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x1AC02000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_lsrv32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x1AC02400 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_asrv32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x1AC02800 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_rorv32(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x1AC02C00 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_lslv64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x9AC02000 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_lsrv64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x9AC02400 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_asrv64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x9AC02800 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_rorv64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, 0x9AC02C00 | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_clz32(Em *e, uint32_t rd, uint32_t rn) {
    em_word(e, 0x5AC01000 | (rn << 5) | rd);
}
static inline void a64_rbit32(Em *e, uint32_t rd, uint32_t rn) {
    em_word(e, 0x5AC00000 | (rn << 5) | rd);
}
static inline void a64_clz64(Em *e, uint32_t rd, uint32_t rn) {
    em_word(e, 0xDAC01000 | (rn << 5) | rd);
}
static inline void a64_rbit64(Em *e, uint32_t rd, uint32_t rn) {
    em_word(e, 0xDAC00000 | (rn << 5) | rd);
}

// ---- float / SIMD scalar
#define VREG(n) (n) // v0-v31
static inline void a64_fldr(Em *e, uint32_t vd, uint32_t rn, int64_t off, int bytes) {
    uint32_t base;
    uint32_t sc;
    switch (bytes) {
    case 4: base = 0xBD400000; sc = 2; break;   // ldr s
    case 8: base = 0xFD400000; sc = 3; break;   // ldr d
    case 16: base = 0x3DC00000; sc = 4; break;  // ldr q
    default: base = 0xBD400000; sc = 2; break;
    }
    if (off >= 0 && (off & (bytes - 1)) == 0 && off < (1 << (12 - 1)) * bytes)
        em_word(e, base | ((uint32_t)(off >> sc) << 10) | (rn << 5) | vd);
    else {
        uint32_t b2;
        switch (bytes) {
        case 4: b2 = 0xBD800000; break;
        case 8: b2 = 0xFD800000; break;
        default: b2 = 0x3DC80000; break;
        }
        em_word(e, b2 | ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | vd);
    }
}
static inline void a64_fstr(Em *e, uint32_t vd, uint32_t rn, int64_t off, int bytes) {
    uint32_t base;
    switch (bytes) {
    case 4: base = 0xBD000000; break;
    case 8: base = 0xFD000000; break;
    default: base = 0x3D800000; break;
    }
    if (off >= 0 && (off & (bytes - 1)) == 0 && off < (1 << 11) * bytes)
        em_word(e, base | ((uint32_t)(off >> (bytes == 4 ? 2 : (bytes == 8 ? 3 : 4))) << 10) | (rn << 5) | vd);
    else {
        em_word(e, (bytes == 4 ? 0xBD800000u : bytes == 8 ? 0xFD800000u : 0x3D880000u) |
                       ((uint32_t)(off & 0x1FF) << 12) | (rn << 5) | vd);
    }
}
static inline void a64_fop(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, int bytes, uint32_t op) {
    uint32_t sz = bytes == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E200000 | sz | op | (rm << 16) | (rn << 5) | rd);
}
// op codes within float data-processing (2 source): fmul=0x00080000>>? — use explicit:
// fmul: 0x0008_0000>>10... simpler: use enums:
#define A64_FADD 0x00002800
#define A64_FSUB 0x00003800
#define A64_FMUL 0x00000800
#define A64_FDIV 0x00001800
#define A64_FMAX 0x00004800
#define A64_FMIN 0x00005800
static inline void a64_fadd(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, int b) { a64_fop(e, rd, rn, rm, b, A64_FADD); }
static inline void a64_fsub(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, int b) { a64_fop(e, rd, rn, rm, b, A64_FSUB); }
static inline void a64_fmul(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, int b) { a64_fop(e, rd, rn, rm, b, A64_FMUL); }
static inline void a64_fdiv(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, int b) { a64_fop(e, rd, rn, rm, b, A64_FDIV); }
static inline void a64_fmax(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, int b) { a64_fop(e, rd, rn, rm, b, A64_FMAX); }
static inline void a64_fmin(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, int b) { a64_fop(e, rd, rn, rm, b, A64_FMIN); }
static inline void a64_fcmp(Em *e, uint32_t rn, uint32_t rm, int b) {
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E200000 | sz | 0x00002000 | (rm << 16) | (rn << 5)); // FCMP has no Rd field
}
static inline void a64_fcmp_zero(Em *e, uint32_t rn, int b) {
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E200000 | sz | 0x00002000 | 8 | (rn << 5) | 0);
}
static inline void a64_fsel(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t cond, int b) {
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E200C00 | sz | (cond << 12) | (rm << 16) | (rn << 5) | rd);
}
static inline void a64_fsqrt(Em *e, uint32_t rd, uint32_t rn, int b) {
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E200000 | sz | 0x0001C000 | (rn << 5) | rd);
}
static inline void a64_frint(Em *e, uint32_t rd, uint32_t rn, int b, uint32_t op) {
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E244000 | sz | op | (rn << 5) | rd);
}
#define A64_FRINTN 0x00000000
#define A64_FRINTP 0x00008000
#define A64_FRINTM 0x00010000
#define A64_FRINTZ 0x00018000
static inline void a64_fabs(Em *e, uint32_t rd, uint32_t rn, int b) {
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E200000 | sz | 0x0000C000 | (rn << 5) | rd);
}
static inline void a64_fneg(Em *e, uint32_t rd, uint32_t rn, int b) {
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E200000 | sz | 0x00014000 | (rn << 5) | rd);
}
static inline void a64_fmov_reg(Em *e, uint32_t rd, uint32_t rn, int b) {
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E200000 | sz | 0x00004000 | (rn << 5) | rd);
}
static inline void a64_fmov_imm(Em *e, uint32_t rd, int b) { // fmov s/d, #0.0
    uint32_t sz = b == 8 ? 0x00400000 : 0;
    em_word(e, 0x1E201000 | sz | rd);
}
static inline void a64_fcvt_sd(Em *e, uint32_t rd, uint32_t rn) { em_word(e, 0x1E22C000 | (rn << 5) | rd); } // s->d
static inline void a64_fcvt_ds(Em *e, uint32_t rd, uint32_t rn) { em_word(e, 0x1E624000 | (rn << 5) | rd); } // d->s
// integer->float conversions (four explicit encodings each)
static inline void a64_scvtf(Em *e, uint32_t rd, uint32_t rn, int dstb, int srcb) {
    // SCVTF Vd, Wn/Xn
    uint32_t base = dstb == 8 ? (srcb == 8 ? 0x9E620000u : 0x1E620000u)
                              : (srcb == 8 ? 0x9E220000u : 0x1E220000u);
    em_word(e, base | (rn << 5) | rd);
}
static inline void a64_ucvtf(Em *e, uint32_t rd, uint32_t rn, int dstb, int srcb) {
    uint32_t base = dstb == 8 ? (srcb == 8 ? 0x9E630000u : 0x1E630000u)
                              : (srcb == 8 ? 0x9E230000u : 0x1E230000u);
    em_word(e, base | (rn << 5) | rd);
}
static inline void a64_fcvtzs(Em *e, uint32_t rd, uint32_t rn, int srcb, int dstb) {
    // FCVTZS Wd/Xd, Sn/Dn  (sf = dstb==8, type: 00=S, 01=D)
    uint32_t base = srcb == 8 ? (dstb == 8 ? 0x9E780000u : 0x1E780000u)
                              : (dstb == 8 ? 0x9E380000u : 0x1E380000u);
    em_word(e, base | (rn << 5) | rd);
}
static inline void a64_fcvtzu(Em *e, uint32_t rd, uint32_t rn, int srcb, int dstb) {
    uint32_t base = srcb == 8 ? (dstb == 8 ? 0x9E790000u : 0x1E790000u)
                              : (dstb == 8 ? 0x9E390000u : 0x1E390000u);
    em_word(e, base | (rn << 5) | rd);
}
static inline void a64_fmov_gpr_fpr(Em *e, uint32_t rd, uint32_t rn, int to_fpr, int b) {
    if (to_fpr) {
        uint32_t base = b == 8 ? 0x9E670000u : 0x1E270000u; // fmov Sd/Dd, Wn/Xn
        em_word(e, base | (rn << 5) | rd);
    } else {
        uint32_t base = b == 8 ? 0x9E660000u : 0x1E260000u; // fmov Wd/Xd, Sn/Dn
        em_word(e, base | (rn << 5) | rd);
    }
}

// ---- FP/SIMD register memory forms (S = 4-byte, D = 8-byte view of V regs)
static inline void a64_ldr_post_fpr(Em *e, uint32_t vt, uint32_t rn, int32_t imm9, int b) {
    em_word(e, (b == 8 ? 0xFC400400u : 0xBC400400u) | ((uint32_t)(imm9 & 0x1FF) << 12) | (rn << 5) | vt);
}
static inline void a64_str_pre_fpr(Em *e, uint32_t vt, uint32_t rn, int32_t imm9, int b) {
    em_word(e, (b == 8 ? 0xFC000C00u : 0xBC000C00u) | ((uint32_t)(imm9 & 0x1FF) << 12) | (rn << 5) | vt);
}
static inline void a64_ldr_fpr_imm(Em *e, uint32_t vt, uint32_t rn, int64_t off, int b) { // scaled imm
    uint32_t sc = (uint32_t)((b == 8 ? off >> 3 : off >> 2) & 0xFFF);
    em_word(e, (b == 8 ? 0xFD400000u : 0xBD400000u) | (sc << 10) | (rn << 5) | vt);
}
static inline void a64_str_fpr_imm(Em *e, uint32_t vt, uint32_t rn, int64_t off, int b) { // scaled imm
    uint32_t sc = (uint32_t)((b == 8 ? off >> 3 : off >> 2) & 0xFFF);
    em_word(e, (b == 8 ? 0xFD000000u : 0xBD000000u) | (sc << 10) | (rn << 5) | vt);
}
static inline void a64_str_reg_fpr(Em *e, uint32_t vt, uint32_t rn, uint32_t rm, int b) {
    em_word(e, (b == 8 ? 0xFC206800u : 0xBC206800u) | (rm << 16) | (rn << 5) | vt);
}
static inline void a64_ldr_reg_fpr(Em *e, uint32_t vt, uint32_t rn, uint32_t rm, int b) {
    em_word(e, (b == 8 ? 0xFC606800u : 0xBC606800u) | (rm << 16) | (rn << 5) | vt);
}

static inline void a64_cset32(Em *e, uint32_t rd, uint32_t cond) {
    // csinc rd, wzr, wzr, invert(cond)
    em_word(e, 0x1A800400 | (((cond ^ 1) & 0xF) << 12) | (31 << 16) | (31 << 5) | rd);
}
static inline void a64_csel64(Em *e, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t cond) {
    em_word(e, 0x9A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
}
static inline void a64_brk(Em *e, uint32_t imm16) {
    em_word(e, 0xD4200000 | ((imm16 & 0xFFFF) << 5));
}
static inline void a64_sxtb32(Em *e, uint32_t rd, uint32_t rn) { em_word(e, 0x13001C00 | (rn << 5) | rd); }
static inline void a64_sxth32(Em *e, uint32_t rd, uint32_t rn) { em_word(e, 0x13003C00 | (rn << 5) | rd); }
static inline void a64_sxtb64(Em *e, uint32_t rd, uint32_t rn) { em_word(e, 0x93401C00 | (rn << 5) | rd); }
static inline void a64_sxth64(Em *e, uint32_t rd, uint32_t rn) { em_word(e, 0x93403C00 | (rn << 5) | rd); }
static inline void a64_sxtw64(Em *e, uint32_t rd, uint32_t rn) { em_word(e, 0x93407C00 | (rn << 5) | rd); }

#endif

// ---- Advanced SIMD (NEON) helpers — words calibrated against llvm-mc ----
// 128-bit slot push/pop (V regs share the GPR load/store encoding space)
static inline void a64_str_q_pre(Em *e, uint32_t rt)  { em_word(e, 0x3C9F0FE0u | rt); }
static inline void a64_ldr_q_post(Em *e, uint32_t rt) { em_word(e, 0x3CC107E0u | rt); }
// q <-> memory, register offset: ldr/str q, [Xn, Xm]
static inline void a64_ldr_q_reg(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) { em_word(e, 0x3CE06800u | (rm << 16) | (rn << 5) | rt); }
static inline void a64_str_q_reg(Em *e, uint32_t rt, uint32_t rn, uint32_t rm) { em_word(e, 0x3CA06800u | (rm << 16) | (rn << 5) | rt); }
// three-same / bitwise op from an llvm-mc-calibrated sample word
// (register fields rm[20:16] / rn[9:5] / rd[4:0] patched in)
static inline void a64_neon(Em *e, uint32_t sample, uint32_t rd, uint32_t rn, uint32_t rm) {
    // clear rd[4:0] / rn[9:5] / rm[20:16] before OR-ing the new registers —
    // an OR over a mask that keeps any register bits produces a bitwise
    // UNION with the calibrated sample's registers (16|2 = 18)
    em_word(e, (sample & 0xFFE0FC00u) | (rm << 16) | (rn << 5) | rd);
}
// dup (splat) from a GPR: se = lane byte width 1/2/4/8
static inline void a64_neon_dup(Em *e, uint32_t se, uint32_t rd, uint32_t rn) {
    em_word(e, (1u << 30) | 0x0E000000u | 0xC00u | (se << 16) | (rn << 5) | rd);
}
// ins Vd.lane <-> GPR (se = lane byte width 1/2/4/8; the imm5 field is
// (lane << (log2(se)+1)) | se, calibrated against llvm-mc per lane width)
static inline uint32_t a64_neon_imm5(uint32_t se, uint32_t lane) {
    static const uint32_t sh[9] = {0, 1, 2, 0, 3, 0, 0, 0, 4}; // 1/2/4/8 indexed
    return ((lane << sh[se]) | se) & 0x1F;
}
static inline void a64_neon_ins(Em *e, uint32_t se, uint32_t lane, uint32_t vd, uint32_t rn) {
    em_word(e, 0x4E001C00u | (a64_neon_imm5(se, lane) << 16) | (rn << 5) | vd);
}
static inline void a64_neon_umov(Em *e, uint32_t se, uint32_t lane, uint32_t rd, uint32_t vn) {
    em_word(e, ((se == 8 ? 1u : 0u) << 30) | 0x0E003C00u | (a64_neon_imm5(se, lane) << 16) | (vn << 5) | rd);
}
static inline void a64_neon_movi0(Em *e, uint32_t rd) { em_word(e, 0x4F00E400u | rd); }

// three-same op with the lane width patched: sample calibrated at .16b
// (size 00) for integer ops or .4s (size 00) for FP; width 0/1/2/3 selects
// 16b/8h/4s/2d (FP: 4s/2d = 0/1)
static inline void a64_neon_w(Em *e, uint32_t sample16b, uint32_t width,
                              uint32_t rd, uint32_t rn, uint32_t rm) {
    em_word(e, (sample16b & ~0x00C00000u) | (width << 22) | (rm << 16) | (rn << 5) | rd);
}
