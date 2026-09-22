// easm aarch64 baseline JIT: near 1:1 wasm->AArch64 instruction translation.
//
// Conventions:
//  - wasm operand stack == native stack, 16-byte slots (interpreter WVal layout);
//  - x25 = memory base, x26 = memory limit (bytes), x27 = EaExec*, x28 = EaInstance*;
//  - x16/x17 scratch; other registers untouched;
//  - explicit bounds checks (guard-page elision is a later optimization);
//  - v128 functions fall back to the interpreter transparently (v1).
#include "easm.h"
#include "opcodes.h"
#include "a64_emit.h"
#include <stdio.h>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

// ---------------------------------------------------------------- code region
static uint8_t *g_code = NULL;
static size_t g_code_len = 0, g_code_cap = 0;

static void jit_wren(int on) {
    pthread_jit_write_protect_np(on ? 0 : 1);
}

static uint8_t *region_alloc(size_t bytes) {
    bytes = (bytes + 15) & ~(size_t)15;
    if (!g_code) {
        void *p = mmap(NULL, 1 << 24, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return NULL;
        g_code = (uint8_t *)p;
        g_code_cap = 1 << 24;
        g_code_len = 0;
    }
    if (g_code_len + bytes > g_code_cap) return NULL; // 16MB exhausted
    uint8_t *r = g_code + g_code_len;
    g_code_len += bytes;
    return r;
}

// ---------------------------------------------------------------- helpers called from JIT code
static void ea_jit_trap_now(EaExec *ex, uint32_t code) {
    ea_trap(ex, (EaTrap)code);
}

static void ea_jit_call_interp(EaExec *ex, EaFuncInst *fi, WVal *args) {
    if (fi->is_host) { // host function: bridge the arg/result buffers directly
        WVal la[16], lr[16];
        uint32_t np = fi->type->n_params < 16 ? fi->type->n_params : 16;
        for (uint32_t k = 0; k < np; k++) la[k] = args[k];
        fi->host_fn(fi->host_user, la, lr);
        uint32_t nr = fi->type->n_results < 16 ? fi->type->n_results : 16;
        for (uint32_t k = 0; k < nr; k++) args[k] = lr[k];
        return;
    }
    uint32_t n = fi->type->n_params;
    uint32_t r = fi->type->n_results;
    // JIT stack layout: wasm arg i lives at args[n-1-i]; push in wasm order
    for (uint32_t k = n; k-- > 0;) ex->stack[ex->sp++] = args[k];
    ex->depth++;
    ea_interp_exec_function(ex, fi);
    ex->depth--;
    // write results back where a JIT callee would leave them: result i at
    // [entry - (i+1)*16] = args[n-1-i]; r > n extends below the args block,
    // which is free red-zone space
    for (uint32_t i = 0; i < r; i++)
        args[(int32_t)n - 1 - (int32_t)i] = ex->stack[ex->sp - 1 - i];
    ex->sp -= r;
}

uint64_t ea_h_popcnt64(uint64_t x) { return __builtin_popcountll(x); }
void ea_h_wdump6(uint64_t fp, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    // fp points at the JIT frame; locals live at [fp-32-16*(nloc-k)] — we
    // don't know nloc here, so dump the raw fp-relative window too
    uint64_t *fpw = (uint64_t *)fp;
    fprintf(stderr, "[R] regs %llu %llu %llu %llu %llu %llu\n",
            (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
            (unsigned long long)d, (unsigned long long)e, (unsigned long long)f);
    fprintf(stderr, "[R] frame -1..-7: %llu %llu %llu %llu %llu %llu %llu\n",
            (unsigned long long)fpw[-1], (unsigned long long)fpw[-2], (unsigned long long)fpw[-3],
            (unsigned long long)fpw[-4], (unsigned long long)fpw[-5], (unsigned long long)fpw[-6],
            (unsigned long long)fpw[-7]);
}

EaFuncInst *ea_jit_callee_lookup(EaExec *ex, EaInstance *inst, uint32_t table_idx,
                                 uint32_t type_idx, uint32_t elem_idx) {
    if (table_idx >= inst->n_tables || elem_idx >= inst->tables[table_idx].size)
        ea_trap(ex, TRAP_UNDEF_ELEM);
    EaFuncInst *fi = (EaFuncInst *)inst->tables[table_idx].elems[elem_idx].ref;
    if (!fi) ea_trap(ex, TRAP_UNINIT_ELEM);
    EaFuncType *want = &inst->module->types[type_idx].func;
    bool sig_ok;
    if (fi->inst && fi->inst->module == inst->module)
        sig_ok = ea_type_sub(inst->module, fi->type_idx, type_idx, 0);
    else
        sig_ok = fi->type->n_params == want->n_params &&
                 fi->type->n_results == want->n_results &&
                 memcmp(fi->type->params, want->params, want->n_params * sizeof(EaValType)) == 0 &&
                 memcmp(fi->type->results, want->results, want->n_results * sizeof(EaValType)) == 0;
    if (!sig_ok)
        ea_trap(ex, TRAP_INDIRECT_TYPE);
    return fi;
}

static WVal *ea_h_memory_size(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx) {
    (void)ex;
    EaMemInst *mem = inst->memories[memidx];
    if (mem->is64) sp[0].i64 = mem->pages; else sp[0].i32 = (uint32_t)mem->pages;
    return sp + 1;
}
static WVal *ea_h_memory_grow(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx) {
    (void)inst;
    EaMemInst *mem = inst->memories[memidx];
    uint64_t delta = mem->is64 ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    uint64_t old;
    if (ea_grow_memory(mem, delta, &old)) {
        if (mem->is64) sp[0].i64 = old; else sp[0].i32 = (uint32_t)old;
    } else {
        if (mem->is64) sp[0].i64 = -1; else sp[0].i32 = 0xFFFFFFFFu;
    }
    return sp;
}
static WVal *ea_h_memory_fill(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx) {
    EaMemInst *mem = inst->memories[memidx];
    uint64_t n = mem->is64 ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    uint8_t byte = (uint8_t)sp[1].i32;
    uint64_t d = mem->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    if (d > mem->size || n > mem->size - d) ea_trap(ex, TRAP_OOB_MEMORY);
    if (n) memset(mem->base + d, byte, n);
    return sp - 3;
}
static WVal *ea_h_memory_copy(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dst, uint32_t src) {
    EaMemInst *md = inst->memories[dst], *ms = inst->memories[src];
    uint64_t n = (md->is64 && ms->is64) ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    uint64_t sv = ms->is64 ? (uint64_t)sp[1].i64 : (uint64_t)(uint32_t)sp[1].i32;
    uint64_t dv = md->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    if (sv > ms->size || n > ms->size - sv) ea_trap(ex, TRAP_OOB_MEMORY);
    if (dv > md->size || n > md->size - dv) ea_trap(ex, TRAP_OOB_MEMORY);
    if (n) memmove(md->base + dv, ms->base + sv, n);
    return sp - 3;
}
static WVal *ea_h_memory_init(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx, uint32_t dataidx) {
    EaMemInst *mem = inst->memories[memidx];
    EaData *d = &inst->module->datas[dataidx];
    uint64_t n = (uint64_t)(uint32_t)sp[0].i32; // len/src always i32
    uint64_t sv = (uint64_t)(uint32_t)sp[1].i32;
    uint64_t dv = mem->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    uint64_t dlen = inst->data_alive[dataidx] ? d->data_len : 0;
    if (sv + n > dlen) ea_trap(ex, TRAP_OOB_MEMORY);
    if (n > mem->size || dv > mem->size - n) ea_trap(ex, TRAP_OOB_MEMORY);
    if (n) memcpy(mem->base + dv, inst->module->owned_bytes + d->data_off + sv, n);
    return sp - 3;
}
static void ea_h_data_drop(EaExec *ex, EaInstance *inst, uint32_t dataidx) {
    (void)ex;
    inst->data_alive[dataidx] = 0;
}
static WVal *ea_h_table_get(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    EaTableInst *t = &inst->tables[tidx];
    uint64_t i = t->is64 ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    if (i >= t->size) ea_trap(ex, TRAP_OOB_TABLE);
    sp[0] = t->elems[i];
    return sp;
}
static WVal *ea_h_table_set(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    EaTableInst *t = &inst->tables[tidx];
    WVal v = sp[0];
    uint64_t i = t->is64 ? (uint64_t)sp[1].i64 : (uint64_t)(uint32_t)sp[1].i32;
    if (i >= t->size) ea_trap(ex, TRAP_OOB_TABLE);
    t->elems[i] = v;
    return sp - 2;
}
static WVal *ea_h_table_size(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    (void)ex;
    EaTableInst *t = &inst->tables[tidx];
    if (t->is64) sp[0].i64 = t->size; else sp[0].i32 = (uint32_t)t->size;
    return sp + 1;
}
static WVal *ea_h_table_grow(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    EaTableInst *t = &inst->tables[tidx];
    uint64_t delta = t->is64 ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    WVal init = sp[1];
    uint64_t old = t->size;
    uint64_t newsz = old + delta;
    if (newsz > t->max || newsz > (1ull << 32)) {
        if (t->is64) sp[0].i64 = -1; else sp[0].i32 = 0xFFFFFFFFu;
    } else {
        t->elems = (WVal *)ea_realloc(t->elems, (newsz ? newsz : 1) * sizeof(WVal));
        for (uint64_t i = old; i < newsz; i++) t->elems[i] = init;
        t->size = newsz;
        if (t->is64) sp[0].i64 = old; else sp[0].i32 = (uint32_t)old;
    }
    return sp;
}
static WVal *ea_h_table_fill(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    EaTableInst *t = &inst->tables[tidx];
    uint64_t n = t->is64 ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    WVal v = sp[1];
    uint64_t d = t->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    if (d > t->size || n > t->size - d) ea_trap(ex, TRAP_OOB_TABLE);
    for (uint64_t i = 0; i < n; i++) t->elems[d + i] = v;
    return sp - 3;
}
static WVal *ea_h_table_copy(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dtn, uint32_t stn) {
    EaTableInst *td = &inst->tables[dtn], *ts = &inst->tables[stn];
    uint64_t n = (td->is64 && ts->is64) ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    uint64_t sv = ts->is64 ? (uint64_t)sp[1].i64 : (uint64_t)(uint32_t)sp[1].i32;
    uint64_t dv = td->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    if (dv > td->size || n > td->size - dv) ea_trap(ex, TRAP_OOB_TABLE);
    if (sv > ts->size || n > ts->size - sv) ea_trap(ex, TRAP_OOB_TABLE);
    memmove(&td->elems[dv], &ts->elems[sv], (size_t)n * sizeof(WVal));
    return sp - 3;
}
static WVal *ea_h_table_init(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx, uint32_t eidx) {
    EaTableInst *t = &inst->tables[tidx];
    EaElem *e = &inst->module->elems[eidx];
    uint64_t n = (uint64_t)(uint32_t)sp[0].i32; // len/src always i32
    uint64_t sv = (uint64_t)(uint32_t)sp[1].i32;
    uint64_t dv = t->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    uint64_t ilen = inst->elem_alive[eidx] ? e->n_items : 0;
    if (sv + n > ilen) ea_trap(ex, TRAP_OOB_TABLE);
    if (n > t->size || dv > t->size - n) ea_trap(ex, TRAP_OOB_TABLE);
    for (uint64_t i = 0; i < n; i++) {
        WVal v;
        if (e->cache) {
            v = e->cache[sv + i];
        } else if (e->items) {
            if (ea_eval_const_expr(inst, inst->module, &e->items[sv + i], &v, e->ref_type) != 0)
                ea_trap(ex, TRAP_OOB_TABLE);
        } else {
            v.ref = &inst->funcs[e->func_idx[sv + i]];
        }
        t->elems[dv + i] = v;
    }
    return sp - 3;
}
static void ea_h_elem_drop(EaExec *ex, EaInstance *inst, uint32_t eidx) {
    (void)ex;
    inst->elem_alive[eidx] = 0;
}
static uint32_t ea_h_f32_min(uint32_t ab, uint32_t bb) {
    float a, b, r;
    memcpy(&a, &ab, 4);
    memcpy(&b, &bb, 4);
    if (a != a) return ab;
    if (b != b) return bb;
    if (a == 0.0f && b == 0.0f) {
        uint32_t ua, ub, rr;
        memcpy(&ua, &ab, 4);
        memcpy(&ub, &bb, 4);
        rr = ((ua | ub) >> 31) ? 0xFF800000u : 0;
        return rr;
    }
    r = a < b ? a : b;
    return *(uint32_t *)&r;
}
static uint32_t ea_h_f32_max(uint32_t ab, uint32_t bb) {
    float a, b, r;
    memcpy(&a, &ab, 4);
    memcpy(&b, &bb, 4);
    if (a != a) return ab;
    if (b != b) return bb;
    if (a == 0.0f && b == 0.0f) {
        uint32_t ua, ub, rr;
        memcpy(&ua, &ab, 4);
        memcpy(&ub, &bb, 4);
        rr = ((ua & ub) >> 31) ? 0xFF800000u : 0;
        return rr;
    }
    r = a > b ? a : b;
    return *(uint32_t *)&r;
}
static uint64_t ea_h_f64_min(uint64_t ab, uint64_t bb) {
    double a, b, r;
    memcpy(&a, &ab, 8);
    memcpy(&b, &bb, 8);
    if (a != a) return ab;
    if (b != b) return bb;
    if (a == 0.0 && b == 0.0) {
        uint64_t ua, ub, rr;
        memcpy(&ua, &ab, 8);
        memcpy(&ub, &bb, 8);
        rr = ((ua | ub) >> 63) ? 0xFFF0000000000000ull : 0;
        return rr;
    }
    r = a < b ? a : b;
    return *(uint64_t *)&r;
}
static uint64_t ea_h_f64_max(uint64_t ab, uint64_t bb) {
    double a, b, r;
    memcpy(&a, &ab, 8);
    memcpy(&b, &bb, 8);
    if (a != a) return ab;
    if (b != b) return bb;
    if (a == 0.0 && b == 0.0) {
        uint64_t ua, ub, rr;
        memcpy(&ua, &ab, 8);
        memcpy(&ub, &bb, 8);
        rr = ((ua & ub) >> 63) ? 0xFFF0000000000000ull : 0;
        return rr;
    }
    r = a > b ? a : b;
    return *(uint64_t *)&r;
}

void ea_dbg_after(uint64_t sp, void *x27v) {
    fprintf(stderr, "DBG after: sp=%llx [sp-16]=%llx x27=%p x28=%p\n",
            (unsigned long long)sp, (unsigned long long)*(uint64_t *)(sp - 16), x27v,
            *(void **)((char *)sp + 8));
}

// ---------------------------------------------------------------- top-level entry trampoline
__attribute__((naked)) void
ea_jit_entry_trampoline(void *entry, EaExec *ex, EaInstance *inst, WVal *args,
                        uint64_t n_args, uint64_t n_results) {
    __asm__ volatile(
        "stp x29, x30, [sp, #-128]!\n"
        "mov x29, sp\n"
        "stp x3, x4, [x29, #16]\n"
        "str x5, [x29, #32]\n"
        // the interpreter's compiled state lives in x19-x28 across this call;
        // JIT code may trash them, so preserve the C ABI contract here
        "stp x19, x20, [x29, #48]\n"
        "stp x21, x22, [x29, #64]\n"
        "stp x23, x24, [x29, #80]\n"
        "stp x25, x26, [x29, #96]\n"
        "stp x27, x28, [x29, #112]\n"
        "sub sp, sp, x4, lsl #4\n"
        "mov x6, #0\n"
        "1:  cmp x6, x4\n"
        "    b.ge 2f\n"
        "    lsl x7, x6, #4\n"
        "    add x9, x3, x7\n"
        "    ldp x10, x11, [x9]\n"
        "    lsl x9, x4, #4\n"
        "    sub x9, x9, #16\n"
        "    sub x9, x9, x7\n"          // dest = sp + (n-1-i)*16 (arg i is the
        "    add x9, sp, x9\n"          // (i+1)-th slot below entry)
        "    stp x10, x11, [x9]\n"
        "    add x6, x6, #1\n"
        "    b 1b\n"
        "2:\n"
        "add sp, sp, x4, lsl #4\n"      // restore sp above the pushed args
        "mov x27, x1\n"
        "mov x28, x2\n"
        "blr x0\n"
        "ldp x3, x4, [x29, #16]\n"
        "ldr x5, [x29, #32]\n"
        "mov x6, #0\n"
        "mov x8, sp\n"                  // callee left sp = entry - R*16, results
        "3:  cmp x6, x5\n"              // at [sp + (R-1-i)*16]
        "    b.ge 4f\n"
        "    lsl x7, x6, #4\n"
        "    lsl x9, x5, #4\n"
        "    sub x9, x9, #16\n"
        "    sub x9, x9, x7\n"
        "    add x9, x9, x8\n"
        "    ldp x10, x11, [x9]\n"
        "    add x9, x3, x7\n"
        "    stp x10, x11, [x9]\n"
        "    add x6, x6, #1\n"
        "    b 3b\n"
        "4:\n"
        "ldp x19, x20, [x29, #48]\n"
        "ldp x21, x22, [x29, #64]\n"
        "ldp x23, x24, [x29, #80]\n"
        "ldp x25, x26, [x29, #96]\n"
        "ldp x27, x28, [x29, #112]\n"
        "mov sp, x29\n"
        "ldp x29, x30, [sp], #128\n"
        "ret\n");
}

__attribute__((naked)) static void trampoline_call(void) {
    __asm__ volatile(
        // x0 = entry, x1 = ex, x2 = inst, x3 = args, x4 = n_args, x5 = n_results
        "stp x29, x30, [sp, #-32]!\n"
        "mov x29, sp\n"
        "blr x0\n"
        "ldp x29, x30, [sp], #32\n"
        "ret\n");
}

int ea_jit_call(EaExec *ex, EaFuncInst *fi) {
    pthread_jit_write_protect_np(1); // ensure executable for this thread
    if (getenv("EA_JIT_STATS")) fprintf(stderr, "ea_jit_call fi=%p entry=%p np=%u first_insn=%08x\n", (void*)fi, fi->jit_entry, fi->type->n_params, *(uint32_t*)fi->jit_entry);
    uint32_t np = fi->type->n_params;
    uint32_t nr = fi->type->n_results;
    if (np + nr > 60) return 1;
    WVal buf[64];
    for (uint32_t i = 0; i < np; i++) buf[i] = ex->stack[ex->sp - np + i];
    ex->sp -= np;
    // direct call: the JIT function keeps x27/x28 set by the trampoline's ABI
    register uint64_t a0 __asm__("x0") = (uint64_t)fi->jit_entry;
    register uint64_t a1 __asm__("x1") = (uint64_t)ex;
    register uint64_t a2 __asm__("x2") = (uint64_t)fi->inst;
    register uint64_t a3 __asm__("x3") = (uint64_t)buf;
    register uint64_t a4 __asm__("x4") = np;
    register uint64_t a5 __asm__("x5") = nr;
    ea_jit_entry_trampoline(fi->jit_entry, ex, fi->inst, buf, np, nr);
    if (getenv("EA_JIT_STATS")) {
        for (uint32_t i = 0; i < nr + 3 && i < 8; i++)
            fprintf(stderr, "  buf[%u]=%016llx\n", i, (unsigned long long)buf[i].i64);
    }
    for (uint32_t i = 0; i < nr; i++) ex->stack[ex->sp++] = buf[i];
    return 0;
}

// ---------------------------------------------------------------- codegen
#define SLOT 16

// float values move directly through S/D registers (no gpr<->fpr round-trip)
#define V0 0
#define V1 1

typedef struct {
    Em em;
    EaModule *m;
    EaFunc *f;
    const EaFuncType *ft;
    uint32_t n_locals, n_params, n_res;
    uint32_t frame_size;
    uint32_t *insn_at;      // pc -> em instruction index (UINT32_MAX unset)
    uint32_t n_pc;
    struct Fx { uint32_t at; uint32_t target_pc; bool cond; uint32_t ccode; } *fx;
    uint32_t nfx, capfx;
    struct TFx { uint32_t at; uint32_t code; bool cond; uint32_t ccode; } *tfx;
    uint32_t ntfx, captfx;
    uint32_t trap_stub_at[16];
    struct {
        uint32_t height, arity, rarity, arity_in;
        uint32_t end_idx, else_idx, block_idx;
        uint8_t is_loop;
    } ctrl[256];
    uint32_t csp;
    uint32_t depth;
    bool reachable;
    int32_t skip_depth;
    uint8_t def_count;      // deferred-operand fusion window (0, 1 or 2 values)
    uint8_t def_kind0;      // width of the deeper deferred value (0 = i32, 1 = 8-byte,
                            // 2 = f32 in v1, 3 = f64 in v1)
    uint8_t def_kind1;      // width of the top deferred value
    uint8_t def_first;      // count==1: value sits in x16 as the first of a pair
    uint32_t cur_pc;
    uint8_t *is_target;     // per-pc: something may branch here
    int8_t fused_cc;        // a comparison set the flags; the next BR_IF must
                            // branch on them (-1 = none)
    uint8_t vpops;          // operands the last pop_pair took from registers
    uint8_t skip_next;      // the next pc (a LOCAL_SET) was fused: skip it
    int16_t cache_map[6];   // local cached in x19-x24 per slot (-1 = none)
    uint8_t cache_dirty;    // bit per slot: register fresher than frame
    uint8_t cache_clock;    // eviction hand
    uint8_t cache_on;       // EA_CACHE/EA_WARM opt-in (stale-value hole under
                            // eviction being chased down)
    // warm-loop two-pass state: pass 1 records the cache state at the loop
    // back-edge; the body is then recompiled with those locals pre-loaded at
    // the head, so loop-carried values never touch memory
    uint32_t wl_pass;       // 0 none, 1 record, 2 warm
    uint32_t fn_idx;        // function being compiled (debug)
    uint32_t wl_head_pc, wl_end_idx, wl_loop_pc;
    uint32_t wl_em0, wl_nfx0, wl_ntfx0;
    int16_t wl_want[6];
    uint32_t wl_n_want;
    char why[80];
    bool failed;
} JC;

static bool compile_scalar_op(JC *c, EaInstr *in);
static bool compile_scalar_op2(JC *c, EaInstr *in);
static bool compile_scalar_op3(JC *c, EaInstr *in);
static void emit_f32_bin(JC *c, uint32_t opc);
static void emit_f64_bin(JC *c, uint32_t opc);
static void emit_f32_minmax(JC *c, bool is_max);
static void emit_f64_minmax(JC *c, bool is_max);
static void emit_trunc(JC *c, const void *helper);
static void fmov_to_fpr(JC *c, uint32_t vd, uint32_t gpr, int b);
static void emit_tail_call(JC *c, bool indirect, EaInstr *in);
static void fmov_to_gpr(JC *c, uint32_t gpr, uint32_t vs, int b);
static void emit_div32(JC *c, bool signed_div, bool want_rem);
static void emit_div64(JC *c, bool signed_div, bool want_rem);
uint64_t ea_h_popcnt64(uint64_t x);
static int32_t ea_h_trunc_i32_f32(EaExec *ex, uint32_t bits);
static uint32_t ea_h_trunc_u32_f32(EaExec *ex, uint32_t bits);
static int32_t ea_h_trunc_i32_f64(EaExec *ex, double f);
static uint32_t ea_h_trunc_u32_f64(EaExec *ex, double f);
static int64_t ea_h_trunc_i64_f32(EaExec *ex, uint32_t bits);
static uint64_t ea_h_trunc_u64_f32(EaExec *ex, uint32_t bits);
static int64_t ea_h_trunc_i64_f64(EaExec *ex, double f);
static uint64_t ea_h_trunc_u64_f64(EaExec *ex, double f);

static void jfail(JC *c, const char *why) {
    if (!c->failed && c->why[0] == 0) snprintf(c->why, sizeof(c->why), "%s", why);
    c->failed = true;
}

static void fix_to_pc(JC *c, uint32_t target_pc) {
    if (c->nfx == c->capfx) {
        c->capfx = c->capfx ? c->capfx * 2 : 64;
        c->fx = (struct Fx *)realloc(c->fx, c->capfx * sizeof(*c->fx));
    }
    c->fx[c->nfx].at = c->em.len - 1;
    c->fx[c->nfx].target_pc = target_pc;
    c->nfx++;
}
static void fix_cond_to_pc(JC *c, uint32_t target_pc, uint32_t ccode) {
    if (c->nfx == c->capfx) {
        c->capfx = c->capfx ? c->capfx * 2 : 64;
        c->fx = (struct Fx *)realloc(c->fx, c->capfx * sizeof(*c->fx));
    }
    c->fx[c->nfx].at = c->em.len - 1;
    c->fx[c->nfx].target_pc = target_pc;
    c->fx[c->nfx].cond = true;
    c->fx[c->nfx].ccode = ccode;
    c->nfx++;
}
static void fix_to_trap(JC *c, uint32_t code) {
    if (c->ntfx == c->captfx) {
        c->captfx = c->captfx ? c->captfx * 2 : 16;
        c->tfx = (struct TFx *)realloc(c->tfx, c->captfx * sizeof(*c->tfx));
    }
    c->tfx[c->ntfx].at = c->em.len - 1;
    c->tfx[c->ntfx].code = code;
    c->tfx[c->ntfx].cond = false;
    c->ntfx++;
}
static void trap_if(JC *c, uint32_t code, uint32_t ccode) {
    em_bcond_label(&c->em, 0, ccode);
    if (c->ntfx == c->captfx) {
        c->captfx = c->captfx ? c->captfx * 2 : 16;
        c->tfx = (struct TFx *)realloc(c->tfx, c->captfx * sizeof(*c->tfx));
    }
    c->tfx[c->ntfx].at = c->em.len - 1;
    c->tfx[c->ntfx].code = code;
    c->tfx[c->ntfx].cond = true;
    c->tfx[c->ntfx].ccode = ccode;
    c->ntfx++;
}
static void b_trap(JC *c, uint32_t code) {
    em_b_label(&c->em, 0);
    fix_to_trap(c, code);
}

// mem base/limit from the shared EaMemInst so growth through any aliasing
// instance is visible at the next call/prologue
static void load_mem_regs(Em *e) {
    a64_ldr_imm64(e, R17, R28, __builtin_offsetof(EaInstance, jit_mem0));
    a64_ldr_imm64(e, R25, R17, __builtin_offsetof(EaMemInst, base));
    a64_ldr_imm64(e, R26, R17, __builtin_offsetof(EaMemInst, size));
}

static void push_x(JC *c, uint32_t rt) { a64_str_pre64(&c->em, rt, SP, -16); }
static void pop_x(JC *c, uint32_t rt) { a64_ldr_post64(&c->em, rt, SP, 16); }
static void push_w(JC *c, uint32_t rt) { a64_str_pre32(&c->em, rt, SP, -16); }
static void pop_w(JC *c, uint32_t rt) { a64_ldr_post32(&c->em, rt, SP, 16); }
// deferred-operand fusion: i32/i64 const or local.get parks its value in x17
// and the very next instruction consumes it from the register instead of the
// stack slot.  Safe because we only defer when no branch can target the
// consumer (is_target pre-scan) and flush at every other instruction.
static bool int_consumes(uint32_t op) {
    switch (op) {
    case EA_OP_I32_ADD: case EA_OP_I32_SUB: case EA_OP_I32_MUL:
    case EA_OP_I32_AND: case EA_OP_I32_OR: case EA_OP_I32_XOR:
    case EA_OP_I32_SHL: case EA_OP_I32_SHR_S: case EA_OP_I32_SHR_U:
    case EA_OP_I32_EQ: case EA_OP_I32_NE:
    case EA_OP_I32_LT_S: case EA_OP_I32_LT_U: case EA_OP_I32_GT_S: case EA_OP_I32_GT_U:
    case EA_OP_I32_LE_S: case EA_OP_I32_LE_U: case EA_OP_I32_GE_S: case EA_OP_I32_GE_U:
    case EA_OP_I64_ADD: case EA_OP_I64_SUB: case EA_OP_I64_MUL:
    case EA_OP_I64_AND: case EA_OP_I64_OR: case EA_OP_I64_XOR:
    case EA_OP_I64_SHL: case EA_OP_I64_SHR_S: case EA_OP_I64_SHR_U:
    case EA_OP_I64_EQ: case EA_OP_I64_NE:
    case EA_OP_I64_LT_S: case EA_OP_I64_LT_U: case EA_OP_I64_GT_S: case EA_OP_I64_GT_U:
    case EA_OP_I64_LE_S: case EA_OP_I64_LE_U: case EA_OP_I64_GE_S: case EA_OP_I64_GE_U:
        return true;
    }
    return false;
}
static bool float_consumes(uint32_t op) {
    switch (op) {
    case EA_OP_F32_ADD: case EA_OP_F32_SUB: case EA_OP_F32_MUL: case EA_OP_F32_DIV:
    case EA_OP_F64_ADD: case EA_OP_F64_SUB: case EA_OP_F64_MUL: case EA_OP_F64_DIV:
        return true;
    }
    return false;
}
// memory loads consume a parked address operand
static bool load_consumes(uint32_t op) {
    return op >= EA_OP_I32_LOAD && op <= EA_OP_I64_LOAD32_U;
}
static bool def_consumes(uint32_t op) {
    return int_consumes(op) || float_consumes(op) || load_consumes(op) ||
           op == EA_OP_F32_STORE || op == EA_OP_F64_STORE;
}
static bool is_def_producer(uint32_t op) {
    return op == EA_OP_I32_CONST || op == EA_OP_I64_CONST || op == EA_OP_LOCAL_GET;
}
static bool defer1_possible(JC *c, uint32_t pc, uint32_t n) {
    return pc + 1 < n && !c->is_target[pc + 1] && def_consumes(c->f->code.v[pc + 1].opcode);
}
static bool defer2_possible(JC *c, uint32_t pc, uint32_t n) {
    return pc + 2 < n && !c->is_target[pc + 1] && !c->is_target[pc + 2] &&
           is_def_producer(c->f->code.v[pc + 1].opcode) &&
           def_consumes(c->f->code.v[pc + 2].opcode);
}
static void flush_deferred(JC *c) {
    if (!c->def_count) return;
    if (c->def_count == 2) { // deeper value first; deferred slots are the ones sp points at
        if (c->def_kind0 == 0) a64_str_pre32(&c->em, R16, SP, -16);
        else a64_str_pre64(&c->em, R16, SP, -16);
    }
    if (c->def_kind1 == 0) a64_str_pre32(&c->em, R17, SP, -16);
    else if (c->def_kind1 == 1) a64_str_pre64(&c->em, R17, SP, -16);
    else if (c->def_kind1 == 2) a64_str_pre_fpr(&c->em, V1, SP, -16, 4);
    else a64_str_pre_fpr(&c->em, V1, SP, -16, 8);
    c->def_count = 0;
    c->def_first = 0;
    c->fused_cc = -1;
    c->skip_next = 0;
}
// fetch both operands of a binary op; with deferred values they are already
// in x16 (first operand) / x17 (second), so nothing is emitted
static void pop_pair_w(JC *c) {
    c->vpops = 0;
    if (c->def_count == 2) { c->def_count = 0; c->vpops = 2; return; }               // a in x16, b in x17
    if (c->def_count == 1) { c->def_count = 0; c->vpops = 1; pop_w(c, R16); return; } // b in x17; pop a
    pop_w(c, R17); pop_w(c, R16);
}
static void pop_pair_x(JC *c) {
    c->vpops = 0;
    if (c->def_count == 2) { c->def_count = 0; c->vpops = 2; return; }
    if (c->def_count == 1) { c->def_count = 0; c->vpops = 1; pop_x(c, R16); return; }
    pop_x(c, R17); pop_x(c, R16);
}
static int32_t local_off(JC *c, uint32_t k);
static void push_s(JC *c, uint32_t vt);
static void push_d(JC *c, uint32_t vt);

// local cache in x19-x24 (callee-saved, so C helpers keep them); the frame
// slot may be stale.  Within loops the warm-backedge restores the recorded
// head state, so loop-carried locals stay in registers across iterations.
#define EA_CACHE_SLOTS 6
static const uint32_t ea_cache_reg[EA_CACHE_SLOTS] = {19, 20, 21, 22, 23, 24};

static void flush_cache(JC *c) {
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
        if (c->cache_map[i] >= 0 && (c->cache_dirty & (1u << i)))
            a64_str_imm64(&c->em, ea_cache_reg[i], FP, local_off(c, (uint32_t)c->cache_map[i]));
        c->cache_map[i] = -1;
    }
    c->cache_dirty = 0;
}
static void load_local_val(JC *c, uint32_t reg, uint32_t idx) {
    if (!c->cache_on) { a64_ldr_imm64(&c->em, reg, FP, local_off(c, idx)); return; }
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
        if (c->cache_map[i] == (int16_t)idx) {
            a64_mov_reg64(&c->em, reg, ea_cache_reg[i]);
            return;
        }
    }
    a64_ldr_imm64(&c->em, reg, FP, local_off(c, idx));
}
static void set_local_val(JC *c, uint32_t idx, uint32_t reg) {
    if (!c->cache_on) { a64_str_imm64(&c->em, reg, FP, local_off(c, idx)); return; }
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
        if (c->cache_map[i] == (int16_t)idx) {
            a64_mov_reg64(&c->em, ea_cache_reg[i], reg);
            c->cache_dirty |= (uint8_t)(1u << i);
            return;
        }
    }
    uint32_t s = EA_CACHE_SLOTS;
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++)
        if (c->cache_map[i] < 0) { s = i; break; }
    if (s == EA_CACHE_SLOTS) {
        for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
            uint32_t k = (c->cache_clock + i) % EA_CACHE_SLOTS;
            if (!(c->cache_dirty & (1u << k))) { s = k; break; }
        }
        if (s == EA_CACHE_SLOTS) {
            s = c->cache_clock % EA_CACHE_SLOTS;
            a64_str_imm64(&c->em, ea_cache_reg[s], FP, local_off(c, (uint32_t)c->cache_map[s]));
        }
    }
    c->cache_clock = (uint8_t)(s + 1);
    a64_mov_reg64(&c->em, ea_cache_reg[s], reg);
    c->cache_map[s] = (int16_t)idx;
    c->cache_dirty |= (uint8_t)(1u << s);
}
// a branch back to the loop head: pass 1 records the cached set; pass 2
// restores EXACTLY that set, with want[j] pinned to slot j (the head was
// compiled with that register assignment)
static void warm_backedge(JC *c) {
    if (getenv("EA_WDBG")) {
        fprintf(stderr, "[B] f%u pass=%u map:", c->fn_idx, c->wl_pass);
        for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++)
            fprintf(stderr, " %d", c->cache_map[i]);
        fprintf(stderr, "\n");
    }
    if (c->wl_pass == 1) {
        c->wl_n_want = 0;
        for (uint32_t i = 0; i < EA_CACHE_SLOTS && c->wl_n_want < EA_CACHE_SLOTS; i++) {
            if (c->cache_map[i] < 0) continue;
            int16_t v = c->cache_map[i];
            bool have = false;
            for (uint32_t j = 0; j < c->wl_n_want; j++) have |= c->wl_want[j] == v;
            if (!have) c->wl_want[c->wl_n_want++] = v;
        }
        flush_cache(c);
        return;
    }
    // write back and free slots holding non-want locals
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
        if (c->cache_map[i] < 0) continue;
        bool in_want = false;
        for (uint32_t j = 0; j < c->wl_n_want; j++)
            in_want |= c->wl_want[j] == c->cache_map[i];
        if (!in_want) {
            if (c->cache_dirty & (1u << i))
                a64_str_imm64(&c->em, ea_cache_reg[i], FP, local_off(c, (uint32_t)c->cache_map[i]));
            c->cache_map[i] = -1;
            c->cache_dirty &= (uint8_t)~(1u << i);
        }
    }
    // pin want[j] to slot j: move or reload as needed
    for (uint32_t j = 0; j < c->wl_n_want; j++) {
        int32_t cur = -1;
        for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++)
            if (c->cache_map[i] == c->wl_want[j]) { cur = (int32_t)i; break; }
        if (cur == (int32_t)j) continue;
        if (c->cache_map[j] >= 0 && (c->cache_dirty & (1u << j)))
            a64_str_imm64(&c->em, ea_cache_reg[j], FP, local_off(c, (uint32_t)c->cache_map[j]));
        if (cur >= 0) {
            a64_mov_reg64(&c->em, ea_cache_reg[j], ea_cache_reg[cur]);
            c->cache_map[j] = c->wl_want[j];
            c->cache_dirty = (uint8_t)((c->cache_dirty & ~(1u << cur)) |
                                       ((c->cache_dirty >> cur) & 1u) << j);
            c->cache_map[cur] = -1;
            c->cache_dirty &= (uint8_t)~(1u << cur);
        } else {
            a64_ldr_imm64(&c->em, ea_cache_reg[j], FP, local_off(c, (uint32_t)c->wl_want[j]));
            c->cache_map[j] = c->wl_want[j];
            c->cache_dirty &= (uint8_t)~(1u << j);
        }
    }
}
static uint32_t cc_invert(uint32_t cc) {
    switch (cc) {
    case CC_EQ: return CC_NE; case CC_NE: return CC_EQ;
    case CC_MI: return CC_PL; case CC_PL: return CC_MI;
    case CC_VS: return CC_VC; case CC_VC: return CC_VS;
    case CC_HI: return CC_LS; case CC_LS: return CC_HI;
    case CC_GE: return CC_LT; case CC_LT: return CC_GE;
    case CC_GT: return CC_LE; case CC_LE: return CC_GT;
    case CC_LO: return CC_HS; case CC_HS: return CC_LO;
    }
    return CC_NE;
}
// the next instruction fuses with a comparison result when it is a BR_IF
// that nothing branches into (flags stay valid, no bool ever hits the stack)
static bool brif_next(JC *c) {
    uint32_t nx = c->cur_pc + 1;
    return nx < (uint32_t)c->f->code.n && !c->is_target[nx] && !c->is_target[c->cur_pc] &&
           c->f->code.v[nx].opcode == EA_OP_BR_IF;
}
// comparison flags live; hand them to the following br_if, else materialize
static void cmp_result_w(JC *c, uint32_t cc) {
    if (brif_next(c)) {
        c->fused_cc = (int8_t)cc;
        // neither the bool nor register-consumed operands occupy a slot
        c->depth -= 1 + c->vpops;
        return;
    }
    a64_cset32(&c->em, R16, cc);
    push_w(&c->em, R16);
}
// binop result in R16: fuse with an adjacent consumer instead of a stack
// round-trip — LOCAL_SET stores straight to the local slot, a following
// binary/comparison op takes the value from x17
static void push_result_w(JC *c) {
    uint32_t nx = c->cur_pc + 1;
    if (nx < (uint32_t)c->f->code.n && !c->is_target[nx]) {
        uint32_t nop = c->f->code.v[nx].opcode;
        if (nop == EA_OP_LOCAL_SET) {
            set_local_val(c, c->f->code.v[nx].imm.u32, R16);
            c->skip_next = 1;
            return;
        }
        if (nop == EA_OP_LOCAL_TEE) {
            a64_str_imm64(&c->em, R16, FP, local_off(c, c->f->code.v[nx].imm.u32));
            push_x(&c->em, R16);
            c->skip_next = 1; // the tee itself is fully fused
            return;
        }
        if (int_consumes(nop) || load_consumes(nop)) {
            a64_mov_reg64(&c->em, R17, R16);
            c->def_kind1 = 1; c->def_count = 1; c->def_first = 0;
            return;
        }
    }
    push_w(&c->em, R16);
}
static void push_result_x(JC *c) {
    uint32_t nx = c->cur_pc + 1;
    if (nx < (uint32_t)c->f->code.n && !c->is_target[nx]) {
        uint32_t nop = c->f->code.v[nx].opcode;
        if (nop == EA_OP_LOCAL_SET) {
            set_local_val(c, c->f->code.v[nx].imm.u32, R16);
            c->skip_next = 1;
            return;
        }
        if (nop == EA_OP_LOCAL_TEE) {
            a64_str_imm64(&c->em, R16, FP, local_off(c, c->f->code.v[nx].imm.u32));
            push_x(&c->em, R16);
            c->skip_next = 1; // the tee itself is fully fused
            return;
        }
        if (int_consumes(nop) || load_consumes(nop)) {
            a64_mov_reg64(&c->em, R17, R16);
            c->def_kind1 = 1; c->def_count = 1; c->def_first = 0;
            return;
        }
    }
    push_x(&c->em, R16);
}
// f32/f64 binop result in v0: park in v1 for a following float op, or store
// straight to a local slot
static void push_result_f(JC *c, int b) {
    uint32_t nx = c->cur_pc + 1;
    if (nx < (uint32_t)c->f->code.n && !c->is_target[nx]) {
        uint32_t nop = c->f->code.v[nx].opcode;
        if (nop == EA_OP_LOCAL_SET) {
            // frame slots sit at negative offsets: round-trip the value
            // through a gpr for the FP-relative store
            fmov_to_gpr(c, R16, V0, b);
            a64_str_imm64(&c->em, R16, FP, local_off(c, c->f->code.v[nx].imm.u32));
            c->skip_next = 1;
            return;
        }
        bool store = b == 4 ? nop == EA_OP_F32_STORE : nop == EA_OP_F64_STORE;
        if (float_consumes(nop) || store) {
            a64_fmov_reg(&c->em, V1, V0, b);
            c->def_kind1 = (uint8_t)(b == 4 ? 2 : 3);
            c->def_count = 1; c->def_first = 0;
            return;
        }
    }
    if (b == 4) push_s(&c->em, V0); else push_d(&c->em, V0);
}
static void push_s(JC *c, uint32_t vt) { a64_str_pre_fpr(&c->em, vt, SP, -16, 4); }
static void pop_s(JC *c, uint32_t vt) { a64_ldr_post_fpr(&c->em, vt, SP, 16, 4); }
static void push_d(JC *c, uint32_t vt) { a64_str_pre_fpr(&c->em, vt, SP, -16, 8); }
static void pop_d(JC *c, uint32_t vt) { a64_ldr_post_fpr(&c->em, vt, SP, 16, 8); }
static void peek_x(JC *c, uint32_t rt, uint32_t slot) { a64_ldr_imm64(&c->em, rt, SP, (int64_t)slot * 16); }
static void poke_x(JC *c, uint32_t rt, uint32_t slot) { a64_str_imm64(&c->em, rt, SP, (int64_t)slot * 16); }

static int32_t local_off(JC *c, uint32_t k) {
    return -32 - (int32_t)(c->n_locals - k) * (int32_t)SLOT;
}
static void call_helper(JC *c, const void *fn) {
    a64_mov64_imm(&c->em, R16, (uint64_t)fn);
    a64_blr(&c->em, R16);
}
static void emit_f32_minmax(JC *c, bool is_max) {
    pop_w(&c->em, R1);
    pop_w(&c->em, R0);
    call_helper(c, is_max ? (const void *)ea_h_f32_max : (const void *)ea_h_f32_min);
    push_w(&c->em, R0);
}
static void emit_f64_minmax(JC *c, bool is_max) {
    pop_x(&c->em, R1);
    pop_x(&c->em, R0);
    call_helper(c, is_max ? (const void *)ea_h_f64_max : (const void *)ea_h_f64_min);
    push_x(&c->em, R0);
}

static void reload_ctx(JC *c) {
    int32_t so = -32 - (int32_t)c->n_locals * 16 - 16;
    a64_ldr_imm64(&c->em, R28, FP, so);
    load_mem_regs(&c->em);
}

static void emit_load(JC *c, EaInstr *in) {
    bool m64 = c->m->memories[in->imm.ma.memidx].is64;
    if (c->def_count == 1 && c->def_kind1 == 1) {
        // address parked in x17 by the preceding integer op
        c->def_count = 0;
        if (m64) a64_mov_reg64(&c->em, R16, R17);
        else a64_mov_reg32(&c->em, R16, R17); // i32 address: zero-extend
    } else {
        if (c->def_count) flush_deferred(c); // e.g. a parked (const,local) pair
        if (m64) pop_x(&c->em, R16);
        else pop_w(&c->em, R16); // 32-bit slots only carry 4 clean bytes
    }
    uint64_t off = in->imm.ma.offset;
    if (off) {
        if (off < 4096) a64_add_imm64(&c->em, R16, R16, (uint32_t)off);
        else { a64_mov64_imm(&c->em, R17, off); a64_add_reg64(&c->em, R16, R16, R17); }
    }
    uint64_t nat;
    switch (in->opcode) {
    case EA_OP_I32_LOAD8_S: case EA_OP_I32_LOAD8_U:
    case EA_OP_I64_LOAD8_S: case EA_OP_I64_LOAD8_U: nat = 1; break;
    case EA_OP_I32_LOAD16_S: case EA_OP_I32_LOAD16_U:
    case EA_OP_I64_LOAD16_S: case EA_OP_I64_LOAD16_U: nat = 2; break;
    case EA_OP_I32_LOAD: case EA_OP_F32_LOAD:
    case EA_OP_I64_LOAD32_S: case EA_OP_I64_LOAD32_U: nat = 4; break;
    default: nat = 8; break;
    }
    // 32-bit memories: out-of-bounds accesses fault inside the 12 GiB
    // PROT_NONE reservation and the signal handler raises the trap, so no
    // explicit check is needed.  64-bit memories have no such bound.
    if (c->m->memories[in->imm.ma.memidx].is64) {
        a64_sub_imm64(&c->em, R17, R26, (uint32_t)nat);
        a64_cmp_reg64(&c->em, R16, R17);
        trap_if(c, TRAP_OOB_MEMORY, CC_HI);
    }
    if (in->opcode == EA_OP_F32_LOAD || in->opcode == EA_OP_F64_LOAD) {
        int b = in->opcode == EA_OP_F64_LOAD ? 8 : 4;
        a64_ldr_reg_fpr(&c->em, V0, R25, R16, b);
        push_result_f(c, b);
        return;
    }
    switch (in->opcode) {
    case EA_OP_I32_LOAD: a64_ldr_reg32(&c->em, R16, R25, R16); break;
    case EA_OP_I64_LOAD: a64_ldr_reg64(&c->em, R16, R25, R16); break;
    case EA_OP_I32_LOAD8_S: a64_ldrsb_reg32(&c->em, R16, R25, R16); break;
    case EA_OP_I32_LOAD8_U: a64_ldrb_reg32(&c->em, R16, R25, R16); break;
    case EA_OP_I32_LOAD16_S: a64_ldrsh_reg32(&c->em, R16, R25, R16); break;
    case EA_OP_I32_LOAD16_U: a64_ldrh_reg32(&c->em, R16, R25, R16); break;
    case EA_OP_I64_LOAD8_S: a64_ldrsb_reg64(&c->em, R16, R25, R16); break;
    case EA_OP_I64_LOAD8_U: a64_ldrb_reg32(&c->em, R16, R25, R16); break;
    case EA_OP_I64_LOAD16_S: a64_ldrsh_reg64(&c->em, R16, R25, R16); break;
    case EA_OP_I64_LOAD16_U: a64_ldrh_reg32(&c->em, R16, R25, R16); break;
    case EA_OP_I64_LOAD32_S: em_word(&c->em, 0xB8A06800 | (R16 << 16) | (R25 << 5) | R16); break;
    case EA_OP_I64_LOAD32_U: a64_ldr_reg32(&c->em, R16, R25, R16); break;
    default: a64_ldr_reg64(&c->em, R16, R25, R16); break;
    }
    push_result_w(c);
}
static void emit_store(JC *c, EaInstr *in) {
    if (in->opcode == EA_OP_F32_STORE || in->opcode == EA_OP_F64_STORE) {
        int b = in->opcode == EA_OP_F64_STORE ? 8 : 4;
        uint32_t vsrc = V0;
        if (c->def_count == 1 && c->def_kind1 == (uint8_t)(b == 4 ? 2 : 3)) {
            c->def_count = 0;
            vsrc = V1; // value parked in v1 by the preceding float op
        } else if (c->def_count) {
            flush_deferred(c); // e.g. a parked (const,local) pair
        }
        if (vsrc == V0) {
            if (b == 8) pop_d(&c->em, V0); else pop_s(&c->em, V0); // value
        }
        if (c->m->memories[in->imm.ma.memidx].is64) pop_x(&c->em, R16); // address
        else pop_w(&c->em, R16); // i32 address: zero-extend
        uint64_t foff = in->imm.ma.offset;
        if (foff) {
            if (foff < 4096) a64_add_imm64(&c->em, R16, R16, (uint32_t)foff);
            else { a64_mov64_imm(&c->em, R17, foff); a64_add_reg64(&c->em, R16, R16, R17); }
        }
        if (c->m->memories[in->imm.ma.memidx].is64) {
            a64_sub_imm64(&c->em, R17, R26, (uint32_t)b);
            a64_cmp_reg64(&c->em, R16, R17);
            trap_if(c, TRAP_OOB_MEMORY, CC_HI);
        }
        a64_str_reg_fpr(&c->em, vsrc, R25, R16, b);
        return;
    }
    pop_x(&c->em, R17); // value
    if (c->m->memories[in->imm.ma.memidx].is64) pop_x(&c->em, R16); // address
    else pop_w(&c->em, R16); // i32 address: 32-bit slots only have 4 clean bytes
    uint64_t off = in->imm.ma.offset;
    if (off) {
        // R17 holds the value; use the free x0 for large offsets
        if (off < 4096) a64_add_imm64(&c->em, R16, R16, (uint32_t)off);
        else { a64_mov64_imm(&c->em, R0, off); a64_add_reg64(&c->em, R16, R16, R0); }
    }
    uint64_t nat;
    switch (in->opcode) {
    case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: nat = 1; break;
    case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: nat = 2; break;
    case EA_OP_I32_STORE: case EA_OP_F32_STORE: case EA_OP_I64_STORE32: nat = 4; break;
    default: nat = 8; break;
    }
    if (c->m->memories[in->imm.ma.memidx].is64) {
        // keep the value; recompute in a free scratch (x0 is free here)
        a64_mov_reg64(&c->em, R0, R17);
        a64_sub_imm64(&c->em, R17, R26, (uint32_t)nat);
        a64_cmp_reg64(&c->em, R16, R17);
        trap_if(c, TRAP_OOB_MEMORY, CC_HI);
        a64_mov_reg64(&c->em, R17, R0);
    }
    switch (in->opcode) {
    case EA_OP_I32_STORE: case EA_OP_F32_STORE: case EA_OP_I64_STORE32:
        a64_str_reg32(&c->em, R17, R25, R16);
        break;
    case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: a64_strb_reg32(&c->em, R17, R25, R16); break;
    case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: a64_strh_reg32(&c->em, R17, R25, R16); break;
    default: a64_str_reg64(&c->em, R17, R25, R16); break;
    }
}

static void ea_h_trace(uint64_t marker) { fprintf(stderr, "JIT-TRACE marker=%llx\n", (unsigned long long)marker); }

static void ea_h_trace2(uint64_t marker, uint64_t sp) {
    fprintf(stderr, "JIT-TRACE2 marker=%llx sp=%llx\n", (unsigned long long)marker, (unsigned long long)sp);
}
static void ea_h_trace3(uint64_t marker, uint64_t slot) {
    fprintf(stderr, "JIT-TRACE3 marker=%llx result=%llx\n", (unsigned long long)marker, (unsigned long long)slot);
}

static void emit_return(JC *c) {
    if (getenv("EA_JIT_TRACE")) {
        a64_mov64_imm(&c->em, R0, 0x5678);
        a64_ldr_imm64(&c->em, R1, SP, 0);
        a64_mov_from_sp(&c->em, R2);
        call_helper(&c->em, (const void *)ea_h_trace3);
    }
    // results on operand stack top: value i is the (i+1)-th slot below entry;
    // copy to [entry_sp-R*16, entry_sp) = [fp+32+below+(A-1-i)*16, ...); then
    // leave sp = entry_sp - R*16 (see call-site contract)
    {
        int32_t below = (c->n_res > c->n_params) ? (int32_t)(c->n_res - c->n_params) * SLOT : 0;
        for (int32_t i = 0; i < (int32_t)c->n_res; i++) {
            int32_t off = 32 + below + (int32_t)(c->n_params - 1 - i) * 16;
            a64_ldr_imm64(&c->em, R16, SP, ((int32_t)c->n_res - 1 - i) * 16);
            a64_str_imm64(&c->em, R16, FP, off);
        }
    }
    a64_mov_sp_from(&c->em, FP);
    a64_ldp_post64(&c->em, FP, LR, SP, 32);
    // ldp restored sp to fp+32 = entry_sp - A*16 - below; land at entry_sp - R*16
    // (below == (R-A)*16 when R > A, so only the A > R case needs an add)
    if (c->n_params > c->n_res)
        a64_add_imm64(&c->em, SP, SP, (c->n_params - c->n_res) * SLOT);
    a64_ret(&c->em);
}

static void emit_call_static(JC *c, EaInstr *in) {
    uint32_t callee = in->imm.u32;
    uint32_t a = 0, r = 0;
    if (callee < c->m->n_funcs) {
        const EaFuncType *t = &c->m->types[c->m->funcs[callee].type_idx].func;
        a = t->n_params;
        r = t->n_results;
    } else {
        jfail(c, "bad callee");
        return;
    }
    uint32_t foff = __builtin_offsetof(EaInstance, funcs);
    uint32_t isz = (uint32_t)sizeof(EaFuncInst);
    a64_ldr_imm64(&c->em, R16, R28, foff);                     // funcs array
    uint64_t byte_off = (uint64_t)callee * isz;
    if (byte_off < 4096) a64_add_imm64(&c->em, R17, R16, (uint32_t)byte_off);
    else {
        a64_mov64_imm(&c->em, R0, byte_off);
        a64_add_reg64(&c->em, R17, R16, R0);
    }
    // x17 = &EaFuncInst
    a64_ldr_imm64(&c->em, R0, R17, __builtin_offsetof(EaFuncInst, inst));
    a64_ldr_imm64(&c->em, R1, R17, __builtin_offsetof(EaFuncInst, jit_entry));
    a64_ldrb_imm32(&c->em, R2, R17, __builtin_offsetof(EaFuncInst, is_jit));
    a64_cmp_imm32(&c->em, R2, 0);
    {
        em_bcond_label(&c->em, 0, CC_EQ);
        uint32_t patch_insn = c->em.len - 1;
        // JIT path: pop the args into the call transition (entry sp = args_end)
        a64_add_imm64(&c->em, SP, SP, a * SLOT);
        a64_mov_reg64(&c->em, R28, R0);
        load_mem_regs(&c->em);
        a64_blr(&c->em, R1);
        // callee left sp = entry - R*16 with results at [sp, sp+R*16); skip the bridge
        em_b_label(&c->em, 0);
        uint32_t skip_at = c->em.len - 1;
        uint32_t join = c->em.len;
        c->em.buf[patch_insn] = 0x54000000 | CC_EQ | (((uint32_t)(join - patch_insn) & 0x7FFFF) << 5);
        // interpreted callee path (sp = args_base here)
        a64_mov_reg64(&c->em, R0, R27);
        a64_mov_reg64(&c->em, R1, R17);
        a64_mov_from_sp(&c->em, R2);
        call_helper(c, (const void *)ea_jit_call_interp);
        if (a > r) a64_add_imm64(&c->em, SP, SP, (a - r) * SLOT);
        else if (r > a) a64_sub_imm64(&c->em, SP, SP, (r - a) * SLOT);
        uint32_t join2 = c->em.len;
        c->em.buf[skip_at] = 0x14000000 | ((join2 - skip_at) & 0x3FFFFFF);
        reload_ctx(c);
    }
}

static void emit_call_indirect(JC *c, EaInstr *in) {
    uint32_t type_idx = in->imm.pair.a, table_idx = in->imm.pair.b;
    const EaFuncType *t = &c->m->types[type_idx].func;
    uint32_t a = t->n_params, r = t->n_results;
    if (c->m->tables[table_idx].is64) pop_x(&c->em, R4); // i64 table index
    else pop_w(&c->em, R4);
    a64_mov_reg64(&c->em, R0, R27);
    a64_mov_reg64(&c->em, R1, R28);
    a64_movz32(&c->em, R2, table_idx & 0xFFFF);
    if (table_idx > 0xFFFF) a64_movk32(&c->em, R2, table_idx >> 16);
    a64_movz32(&c->em, R3, type_idx & 0xFFFF);
    if (type_idx > 0xFFFF) a64_movk32(&c->em, R3, type_idx >> 16);
    call_helper(c, (const void *)ea_jit_callee_lookup);
    // x0 = fi
    a64_mov_reg64(&c->em, R17, R0);
    a64_ldr_imm64(&c->em, R0, R17, __builtin_offsetof(EaFuncInst, inst));
    a64_ldr_imm64(&c->em, R1, R17, __builtin_offsetof(EaFuncInst, jit_entry));
    a64_ldrb_imm32(&c->em, R2, R17, __builtin_offsetof(EaFuncInst, is_jit));
    a64_cmp_imm32(&c->em, R2, 0);
    {
        em_bcond_label(&c->em, 0, CC_EQ);
        uint32_t patch_insn = c->em.len - 1;
        a64_mov_reg64(&c->em, R28, R0);
        load_mem_regs(&c->em);
        a64_blr(&c->em, R1);
        // callee left sp = entry - R*16 with results at [sp, sp+R*16); skip the bridge
        em_b_label(&c->em, 0);
        uint32_t skip_at = c->em.len - 1;
        uint32_t join = c->em.len;
        c->em.buf[patch_insn] = 0x54000000 | CC_EQ | (((uint32_t)(join - patch_insn) & 0x7FFFF) << 5);
        a64_mov_reg64(&c->em, R0, R27);
        a64_mov_reg64(&c->em, R1, R17);
        a64_mov_from_sp(&c->em, R2);
        call_helper(c, (const void *)ea_jit_call_interp);
        if (a > r) a64_add_imm64(&c->em, SP, SP, (a - r) * SLOT);
        else if (r > a) a64_sub_imm64(&c->em, SP, SP, (r - a) * SLOT);
        uint32_t join2 = c->em.len;
        c->em.buf[skip_at] = 0x14000000 | ((join2 - skip_at) & 0x3FFFFFF);
        reload_ctx(c);
    }
}

static void emit_fcmp32(JC *c, uint32_t cond, bool use_nan_true) {
    pop_s(&c->em, V1);
    pop_s(&c->em, V0);
    a64_fcmp(&c->em, V0, V1, 4);
    uint32_t fcond;
    switch (cond) {
    case CC_EQ: fcond = CC_EQ; break;
    case CC_NE: fcond = CC_NE; break;
    case CC_LT: fcond = CC_MI; break;
    case CC_GT: fcond = CC_GT; break;
    case CC_LE: fcond = CC_LS; break;
    default: fcond = CC_GE; break;
    }
    if (use_nan_true && cond == CC_NE) fcond = CC_NE;
    cmp_result_w(c, fcond);
}
static void emit_fcmp64(JC *c, uint32_t cond) {
    pop_d(&c->em, V1);
    pop_d(&c->em, V0);
    a64_fcmp(&c->em, V0, V1, 8);
    uint32_t fcond;
    switch (cond) {
    case CC_EQ: fcond = CC_EQ; break;
    case CC_NE: fcond = CC_NE; break;
    case CC_LT: fcond = CC_MI; break;
    case CC_GT: fcond = CC_GT; break;
    case CC_LE: fcond = CC_LS; break;
    default: fcond = CC_GE; break;
    }
    a64_cset32(&c->em, R16, fcond);
    push_w(&c->em, R16);
}

static void emit_helper3(JC *c, const void *helper, uint32_t imm0, uint32_t imm1) {
    a64_mov_reg64(&c->em, R0, R27);
    a64_mov_reg64(&c->em, R1, R28);
    a64_mov_from_sp(&c->em, R2);
    a64_movz32(&c->em, R3, imm0 & 0xFFFF);
    if (imm0 > 0xFFFF) a64_movk32(&c->em, R3, imm0 >> 16);
    a64_movz32(&c->em, R4, imm1 & 0xFFFF);
    if (imm1 > 0xFFFF) a64_movk32(&c->em, R4, imm1 >> 16);
    call_helper(c, helper);
    a64_mov_sp_from(&c->em, R0);   // new sp (add form: orr cannot write sp)
    reload_ctx(c);
}

static void br_label_target(JC *c, uint32_t l, uint32_t *theight, uint32_t *tarity,
                            bool *tloop, uint32_t *tpc) {
    if (l >= c->csp) { // function-level label: return with the function's results
        *theight = 0; *tarity = c->n_res; *tloop = false; *tpc = c->n_pc;
        return;
    }
    *theight = c->ctrl[c->csp - 1 - l].height;
    *tarity = c->ctrl[c->csp - 1 - l].arity;
    *tloop = c->ctrl[c->csp - 1 - l].is_loop != 0;
    *tpc = *tloop ? c->ctrl[c->csp - 1 - l].block_idx + 1 : c->ctrl[c->csp - 1 - l].end_idx;
}

static void emit_br_to(JC *c, uint32_t cur, uint32_t target_depth, uint32_t arity) {
    // carry the top `arity` slots (absolute [cur-arity+1, cur]) to absolute
    // [target_depth, target_depth+arity), then land sp at slot target_depth.
    // [sp + k*16] is absolute slot (cur - k), so:
    //   src slot (cur-arity+1+i) -> [sp + (arity-1-i)*16]
    //   dst slot (target_depth+i) -> [sp + (cur-target_depth-i)*16]
    for (int32_t i = (int32_t)arity - 1; i >= 0; i--) {
        a64_ldr_imm64(&c->em, R16, SP, ((int64_t)arity - 1 - i) * 16);
        a64_str_imm64(&c->em, R16, SP, ((int64_t)cur - (int64_t)target_depth - i) * 16);
    }
    int32_t up = (int32_t)(cur - target_depth); // slots the pointer moves up
    if (up > 0) a64_add_imm64(&c->em, SP, SP, up * SLOT);
    else if (up < 0) a64_sub_imm64(&c->em, SP, SP, (-up) * SLOT);
}

static void compile_loop_body_op(JC *c, uint32_t pc);

static bool compile_function(JC *c) {
    EaFunc *f = c->f;
    InsList *code = &f->code;
    uint32_t n = code->n;
    c->n_pc = n + 1; // label space for the implicit end
    c->insn_at = (uint32_t *)malloc((c->n_pc + 1) * 4);
    for (uint32_t i = 0; i <= c->n_pc; i++) c->insn_at[i] = UINT32_MAX;
    c->failed = false;
    c->why[0] = 0;
    c->csp = 0;
    c->depth = 0;
    c->reachable = true;
    c->skip_depth = -1;
    c->def_count = 0;
    c->def_first = 0;
    c->fused_cc = -1;
    c->skip_next = 0;
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) c->cache_map[i] = -1;
    c->cache_dirty = 0;
    c->cache_clock = 0;
    c->wl_pass = 0;
    c->wl_n_want = 0;
    c->cache_on = (getenv("EA_CACHE") != NULL || getenv("EA_WARM") != NULL);
    c->is_target = (uint8_t *)calloc(n + 2, 1);
    if (c->is_target)
        for (uint32_t i = 0; i < n; i++) {
            EaInstr *ti = &code->v[i];
            if (ti->opcode == EA_OP_BLOCK || ti->opcode == EA_OP_LOOP || ti->opcode == EA_OP_IF) {
                if (ti->end_idx <= n) c->is_target[ti->end_idx] = 1;
                if (ti->opcode == EA_OP_LOOP && i + 1 <= n) c->is_target[i + 1] = 1;
                if (ti->opcode == EA_OP_IF && ti->else_idx != UINT32_MAX && ti->else_idx + 1 <= n)
                    c->is_target[ti->else_idx + 1] = 1;
            }
        }
    c->is_target[n] = 1;

    Em *e = &c->em;
    // prologue: skip past the incoming args area, and when R > A also reserve
    // slots for the results (they land at [entry_sp-R*16, entry_sp), which
    // would otherwise overlap this frame); then build our frame below it
    {
        int32_t skip = (int32_t)c->n_params * SLOT;
        if (c->n_res > c->n_params) skip += (int32_t)(c->n_res - c->n_params) * SLOT;
        if (skip) a64_sub_imm64(e, SP, SP, skip);
    }
    a64_stp_pre64(e, FP, LR, SP, -32);
    a64_mov_from_sp(e, FP);
    a64_sub_imm64(e, SP, SP, c->frame_size);
    {
        // save instance below the locals (never collides with args or operands)
        int32_t so = -32 - (int32_t)c->n_locals * 16 - 16;
        a64_str_imm64(e, R28, FP, so);
    }
    // load mem base/limit
    load_mem_regs(e);
    if (getenv("EA_JIT_TRACE")) {
        a64_mov64_imm(e, R0, 0x1234);
        a64_mov_from_sp(e, R1);
        call_helper(c, (const void *)ea_h_trace);
    }
    // copy params from args area into local slots
    // (arg i is the (i+1)-th slot below entry: [fp + 32 + below + (A-1-i)*16])
    {
        int32_t below = (c->n_res > c->n_params) ? (int32_t)(c->n_res - c->n_params) * SLOT : 0;
        for (uint32_t i = 0; i < c->n_params && i < c->n_locals; i++) {
            a64_ldr_imm64(e, R16, FP, 32 + below + (int32_t)(c->n_params - 1 - i) * 16);
            a64_str_imm64(e, R16, FP, local_off(c, i));
        }
        // wasm zeroes declared locals; the frame is raw stack memory
        for (uint32_t i = c->n_params; i < c->n_locals; i++)
            a64_str_imm64(e, 31, FP, local_off(c, i));
    }

    uint32_t pc = 0;
    while (pc < n) {
        if (c->skip_next) { // result stored straight to a local slot
            c->skip_next = 0;
            pc++;
            continue;
        }
        c->insn_at[pc] = e->len;
        // the register file caching locals lives only inside straight-line
        // runs; AFTER insn_at so branches targeting this label execute the
        // write-backs (BR/BR_IF resolve their target first and may keep a
        // warm loop state)
        switch (code->v[pc].opcode) {
        case EA_OP_BR_TABLE:
        case EA_OP_RETURN:
        case EA_OP_CALL: case EA_OP_CALL_INDIRECT: case EA_OP_CALL_REF:
        case EA_OP_RETURN_CALL: case EA_OP_RETURN_CALL_INDIRECT:
        case EA_OP_RETURN_CALL_REF:
        case EA_OP_BLOCK: case EA_OP_LOOP: case EA_OP_IF:
        case EA_OP_ELSE: case EA_OP_END:
            flush_cache(c);
            break;
        }
        c->depth = c->f->depths[pc];
        c->cur_pc = pc;
        EaInstr *in = &code->v[pc];
        uint32_t op = in->opcode;
        if (c->def_count) {
            bool keep = c->skip_depth < 0 && c->reachable &&
                        (def_consumes(op) ||
                         (c->def_count == 1 && c->def_first &&
                          is_def_producer(op) && defer1_possible(c, pc, n)));
            if (!keep) flush_deferred(c);
        }
        if (c->skip_depth >= 0) {
            // skipping unreachable code
            if (op == EA_OP_BLOCK || op == EA_OP_LOOP || op == EA_OP_IF) {
                c->skip_depth++;
                pc++;
                continue;
            }
            if (op == EA_OP_END) {
                if (c->skip_depth > 0) { c->skip_depth--; pc++; continue; }
                if (c->csp == 0) {
                    // function-level end after unreachable: no reachable tail
                    c->skip_depth = -1;
                    c->reachable = false;
                    pc++;
                    continue;
                }
                // join at block end
                c->depth = c->ctrl[c->csp - 1].height + c->ctrl[c->csp - 1].rarity;
                c->csp--;
                c->skip_depth = -1;
                c->reachable = true;
                // the warm loop's END can be reached in skip mode (its body
                // exited via an interior br); finalize the wl context here or
                // it stays stuck and later branches corrupt registers
                if (c->wl_pass && pc == c->wl_end_idx) {
                    c->wl_pass = 0;
                    flush_cache(c);
                }
                pc++;
                continue;
            }
            if (op == EA_OP_ELSE && c->skip_depth == 0) {
                c->depth = c->ctrl[c->csp - 1].height + c->ctrl[c->csp - 1].arity_in;
                c->skip_depth = -1;
                c->reachable = true;
                pc++;
                continue;
            }
            pc++;
            continue;
        }
        switch (op) {
        case EA_OP_NOP: break;
        case EA_OP_UNREACHABLE: c->skip_depth = 0; c->reachable = false; break;
        case EA_OP_BLOCK:
            c->ctrl[c->csp].height = in->height;
            c->ctrl[c->csp].arity = in->arity_out;
            c->ctrl[c->csp].rarity = in->arity_res;
            c->ctrl[c->csp].arity_in = in->arity_in;
            c->ctrl[c->csp].end_idx = in->end_idx;
            c->ctrl[c->csp].else_idx = UINT32_MAX;
            c->ctrl[c->csp].block_idx = pc;
            c->ctrl[c->csp].is_loop = 0;
            c->csp++;
            break;
        case EA_OP_LOOP:
            // warm-loop two-pass: correct on the spec suite but a stale-value
            // hole remains for >6-live-local unrolled bodies (bench kernels);
            // EA_WARM=1 opts in while that is being chased down
            if (c->wl_pass == 0 && c->cache_on && getenv("EA_WARM") != NULL) {
                if (getenv("EA_WDBG")) fprintf(stderr, "[L] f%u loop pc=%u head=%u end=%u nloc=%u\n", c->fn_idx, pc, pc + 1, in->end_idx, c->n_locals);
                c->wl_pass = 1;
                c->wl_head_pc = pc + 1;
                c->wl_end_idx = in->end_idx;
                c->wl_loop_pc = pc;
                c->wl_em0 = c->em.len;
                c->wl_nfx0 = c->nfx;
                c->wl_ntfx0 = c->ntfx;
                c->wl_n_want = 0;
            }
            c->ctrl[c->csp].height = in->height;
            c->ctrl[c->csp].arity = in->arity_out;
            c->ctrl[c->csp].rarity = in->arity_res;
            c->ctrl[c->csp].arity_in = in->arity_in;
            c->ctrl[c->csp].end_idx = in->end_idx;
            c->ctrl[c->csp].else_idx = UINT32_MAX;
            c->ctrl[c->csp].block_idx = pc;
            c->ctrl[c->csp].is_loop = 1;
            c->csp++;
            break;
        case EA_OP_IF: {
            pop_w(e == e ? &c->em : e, R16); // condition
            c->ctrl[c->csp].height = in->height;
            c->ctrl[c->csp].arity = in->arity_out;
            c->ctrl[c->csp].rarity = in->arity_res;
            c->ctrl[c->csp].arity_in = in->arity_in;
            c->ctrl[c->csp].end_idx = in->end_idx;
            c->ctrl[c->csp].else_idx = in->else_idx;
            c->ctrl[c->csp].block_idx = pc;
            c->ctrl[c->csp].is_loop = 0;
            c->csp++;
            uint32_t jtarget = (in->else_idx != UINT32_MAX) ? in->else_idx + 1 : in->end_idx;
            a64_cmp_imm32(e, R16, 0);
            em_bcond_label(e, 0, CC_EQ);
            fix_cond_to_pc(c, jtarget, CC_EQ);
            break;
        }
        case EA_OP_ELSE:
            // taken-branch fallthrough: jump to end
            c->depth = c->ctrl[c->csp - 1].height + c->ctrl[c->csp - 1].rarity;
            {
                em_b_label(e, 0);
                fix_to_pc(c, c->ctrl[c->csp - 1].end_idx);
            }
            pc = c->ctrl[c->csp - 1].end_idx; // emit join label there
            continue;                          // skip pc++ (label recorded next iteration)
        case EA_OP_END:
            if (c->csp > 0) {
                c->depth = c->ctrl[c->csp - 1].height + c->ctrl[c->csp - 1].rarity;
                c->csp--;
            }
            if (getenv("EA_WDBG")) fprintf(stderr, "[E] end pc=%u wl_end=%u pass=%u\n", pc, c->wl_end_idx, c->wl_pass);
            if (c->wl_pass && pc == c->wl_end_idx) {
                if (c->wl_pass == 1 && c->wl_n_want > 0) {
                    // pass 2: discard the body, restart at the head with the
                    // recorded locals pre-loaded into registers
                    c->em.len = c->wl_em0;
                    c->nfx = c->wl_nfx0;
                    c->ntfx = c->wl_ntfx0;
                    for (uint32_t i = 0; i < c->wl_n_want && i < EA_CACHE_SLOTS; i++) {
                        a64_ldr_imm64(e, ea_cache_reg[i], FP, local_off(c, (uint32_t)c->wl_want[i]));
                        c->cache_map[i] = c->wl_want[i];
                    }
                    c->cache_dirty = 0;
                    if (getenv("EA_WDBG2")) {
                        for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++)
                            a64_mov_reg64(e, R1 + i, ea_cache_reg[i]);
                        a64_mov_reg64(e, R0, FP);
                        call_helper(c, (const void *)ea_h_wdump6);
                    }
                    EaInstr *lin = &code->v[c->wl_loop_pc];
                    c->ctrl[c->csp].height = lin->height;
                    c->ctrl[c->csp].arity = lin->arity_out;
                    c->ctrl[c->csp].rarity = lin->arity_res;
                    c->ctrl[c->csp].arity_in = lin->arity_in;
                    c->ctrl[c->csp].end_idx = lin->end_idx;
                    c->ctrl[c->csp].else_idx = UINT32_MAX;
                    c->ctrl[c->csp].block_idx = c->wl_loop_pc;
                    c->ctrl[c->csp].is_loop = 1;
                    c->csp++;
                    c->wl_pass = 2;
                    c->reachable = true;  // the pass-1 body ended in a br
                    c->skip_depth = -1;
                    pc = c->wl_head_pc - 1; // pc++ lands on the head
                    break;
                }
                if (c->wl_pass == 2 && getenv("EA_WDBG2")) {
                    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++)
                        a64_mov_reg64(e, R1 + i, ea_cache_reg[i]);
                    a64_mov_reg64(e, R0, FP);
                    call_helper(c, (const void *)ea_h_wdump6);
                }
                c->wl_pass = 0;
            }
            break;
        case EA_OP_BR: {
            uint32_t l = in->imm.u32;
            uint32_t theight, tarity, tpc;
            bool tloop;
            if (l >= c->csp) {
                // function-level label: return with the function's results
                theight = 0;
                tarity = c->n_res;
                tloop = false;
                tpc = c->n_pc; // implicit end (emit_return tail)
            } else {
                theight = c->ctrl[c->csp - 1 - l].height;
                tarity = c->ctrl[c->csp - 1 - l].arity;
                tloop = c->ctrl[c->csp - 1 - l].is_loop != 0;
                tpc = tloop ? c->ctrl[c->csp - 1 - l].block_idx + 1
                            : c->ctrl[c->csp - 1 - l].end_idx;
            }
            if (c->wl_pass && tpc == c->wl_head_pc) warm_backedge(c);
            else flush_cache(c);
            emit_br_to(c, c->depth, theight + tarity, tarity);
            em_b_label(e, 0);
            fix_to_pc(c, tpc);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        }
        case EA_OP_BR_IF: {
            uint32_t l = in->imm.u32;
            uint32_t theight, tarity, tpc;
            bool tloop;
            if (l >= c->csp) {
                theight = 0;
                tarity = c->n_res;
                tloop = false;
                tpc = c->n_pc;
            } else {
                theight = c->ctrl[c->csp - 1 - l].height;
                tarity = c->ctrl[c->csp - 1 - l].arity;
                tloop = c->ctrl[c->csp - 1 - l].is_loop != 0;
                tpc = tloop ? c->ctrl[c->csp - 1 - l].block_idx + 1
                            : c->ctrl[c->csp - 1 - l].end_idx;
            }
            // emitted before the branch so both paths establish the state the
            // compile-time cache map describes
            if (getenv("EA_WDBG")) fprintf(stderr, "[X] f%u brif pc=%u tpc=%u head=%u pass=%u csp=%u\n", c->fn_idx, pc, tpc, c->wl_head_pc, c->wl_pass, c->csp);
            if (c->wl_pass && tpc == c->wl_head_pc) warm_backedge(c);
            else flush_cache(c);
            uint32_t cc;
            if (c->fused_cc >= 0) {
                // comparison set the flags directly: branch on them, the bool
                // was never materialized
                cc = cc_invert((uint32_t)c->fused_cc);
                c->fused_cc = -1; // depth was corrected by the comparison
            } else {
                pop_w(e, R16); // cond
                a64_cmp_imm32(e, R16, 0);
                cc = CC_EQ;
            }
            em_bcond_label(e, 0, cc);
            fix_cond_to_pc(c, pc + 1, cc); // fallthrough when zero/false
            emit_br_to(c, c->depth - 1, theight + tarity, tarity); // cond already popped
            em_b_label(e, 0);
            fix_to_pc(c, tpc);
            // taken path jumped away; fallthrough remains reachable
            break;
        }
        case EA_OP_BR_TABLE: {
            // index in R16; comparison chain over the non-default labels, each
            // case does its own stack fixup (labels may differ in arity)
            pop_w(e, R16); // index
            uint32_t base = in->imm.pair.a, ntbl = in->imm.pair.b;
            uint32_t *pool = code->pool;
            uint32_t *at = (uint32_t *)malloc((ntbl + 1) * 4); // b.eq sites + default branch
            if (!at) { jfail(c, "oom"); break; }
            for (uint32_t i = 0; i < ntbl; i++) {
                if (i < 4096) a64_cmp_imm32(e, R16, i);
                else { a64_mov32_imm(e, R17, i); a64_cmp_reg32(e, R16, R17); }
                em_bcond_label(e, 0, CC_EQ);
                at[i] = c->em.len - 1;
            }
            em_b_label(e, 0);
            at[ntbl] = c->em.len - 1; // branch to the default case
            for (uint32_t i = 0; i < ntbl; i++) {
                uint32_t case_at = c->em.len, th, ta, tpc;
                bool tl;
                br_label_target(c, pool[base + i], &th, &ta, &tl, &tpc);
                emit_br_to(c, c->depth - 1, th + ta, ta);
                em_b_label(e, 0);
                fix_to_pc(c, tpc);
                c->em.buf[at[i]] = 0x54000000 | CC_EQ | (((case_at - at[i]) & 0x7FFFF) << 5);
            }
            {
                uint32_t case_at = c->em.len, th, ta, tpc;
                bool tl;
                br_label_target(c, pool[base + ntbl], &th, &ta, &tl, &tpc);
                emit_br_to(c, c->depth - 1, th + ta, ta);
                em_b_label(e, 0);
                fix_to_pc(c, tpc);
                c->em.buf[at[ntbl]] = 0x14000000 | ((case_at - at[ntbl]) & 0x3FFFFFF);
            }
            free(at);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        }
        case EA_OP_RETURN:
            emit_return(c);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        case EA_OP_RETURN_CALL:
            emit_tail_call(c, false, in);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        case EA_OP_RETURN_CALL_INDIRECT:
            emit_tail_call(c, true, in);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        case EA_OP_CALL:
            emit_call_static(c, in);
            break;
        case EA_OP_CALL_INDIRECT:
            emit_call_indirect(c, in);
            break;
        case EA_OP_DROP:
            a64_add_imm64(e, SP, SP, SLOT);
            break;
        case EA_OP_SELECT: {
            pop_w(e, R16); // cond
            pop_x(e, R17); // false value
            pop_x(e, R0);  // true value
            a64_cmp_imm32(e, R16, 0);
            // csinc-based select: use csel x17, x0, x17, ne
            a64_csel64(e, R17, R0, R17, CC_NE); // select: cond!=0 -> R0 (true) else R17
            push_x(e, R17);
            break;
        }
        case EA_OP_SELECT_T:
            pop_w(e, R16);
            pop_x(e, R17);
            pop_x(e, R0);
            a64_cmp_imm32(e, R16, 0);
            em_word(e, 0x9EA00C00 | (R0 << 16) | (CC_NE << 12) | (R17 << 5) | R17);
            push_x(e, R17);
            break;
        case EA_OP_LOCAL_GET:
            if (c->def_count == 1 && c->def_first) {
                load_local_val(c, R17, in->imm.u32);
                c->def_kind1 = 1; c->def_count = 2; c->def_first = 0;
            } else if (c->def_count == 0 && defer1_possible(c, pc, n)) {
                load_local_val(c, R17, in->imm.u32);
                c->def_kind1 = 1; c->def_count = 1;
            } else if (c->def_count == 0 && defer2_possible(c, pc, n)) {
                load_local_val(c, R16, in->imm.u32);
                c->def_kind0 = 1; c->def_count = 1; c->def_first = 1;
            } else {
                load_local_val(c, R16, in->imm.u32);
                push_x(e, R16);
            }
            break;
        case EA_OP_LOCAL_SET:
            pop_x(e, R16);
            set_local_val(c, in->imm.u32, R16);
            break;
        case EA_OP_LOCAL_TEE:
            peek_x(e, R16, 0);
            set_local_val(c, in->imm.u32, R16);
            break;
        case EA_OP_GLOBAL_GET:
            a64_ldr_imm64(e, R16, R28, __builtin_offsetof(EaInstance, jit_globals));
            a64_ldr_imm64(e, R16, R16, (int64_t)in->imm.u32 * 8);
            a64_ldr_imm64(e, R16, R16, 0);
            push_x(e, R16);
            break;
        case EA_OP_GLOBAL_SET:
            pop_x(e, R16);
            a64_ldr_imm64(e, R17, R28, __builtin_offsetof(EaInstance, jit_globals));
            a64_ldr_imm64(e, R17, R17, (int64_t)in->imm.u32 * 8);
            a64_str_imm64(e, R16, R17, 0);
            break;
        case EA_OP_MEMORY_SIZE:
            emit_helper3(c, ea_h_memory_size, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_MEMORY_GROW:
            emit_helper3(c, ea_h_memory_grow, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_MEMORY_FILL:
            emit_helper3(c, ea_h_memory_fill, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_MEMORY_COPY:
            emit_helper3(c, ea_h_memory_copy, in->imm.pair.a, in->imm.pair.b);
            break;
        case EA_OP_MEMORY_INIT:
            emit_helper3(c, ea_h_memory_init, in->imm.pair.a, in->imm.pair.b);
            break;
        case EA_OP_DATA_DROP:
            emit_helper3(c, ea_h_data_drop, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_TABLE_GET:
            emit_helper3(c, ea_h_table_get, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_TABLE_SET:
            emit_helper3(c, ea_h_table_set, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_TABLE_SIZE:
            emit_helper3(c, ea_h_table_size, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_TABLE_GROW:
            emit_helper3(c, ea_h_table_grow, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_TABLE_FILL:
            emit_helper3(c, ea_h_table_fill, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_TABLE_COPY:
            emit_helper3(c, ea_h_table_copy, in->imm.pair.a, in->imm.pair.b);
            break;
        case EA_OP_TABLE_INIT:
            emit_helper3(c, ea_h_table_init, in->imm.pair.a, in->imm.pair.b);
            break;
        case EA_OP_ELEM_DROP:
            emit_helper3(c, ea_h_elem_drop, in->imm.u32, 0xFFFFFFFFu);
            break;
        case EA_OP_REF_NULL: {
            a64_mov_reg64(e, R16, 31);
            push_x(e, R16);
            break;
        }
        case EA_OP_REF_FUNC: {
            uint32_t foff = __builtin_offsetof(EaInstance, funcs);
            a64_ldr_imm64(e, R16, R28, foff);
            uint64_t bo = (uint64_t)in->imm.u32 * sizeof(EaFuncInst);
            if (bo < 4096) a64_add_imm64(e, R16, R16, (uint32_t)bo);
            else {
                a64_mov64_imm(e, R0, bo);
                a64_add_reg64(e, R16, R16, R0);
            }
            push_x(e, R16);
            break;
        }
        case EA_OP_REF_IS_NULL: {
            peek_x(e, R16, 0);
            a64_cmp_reg64(e, R16, 31);
            a64_cset32(e, R16, CC_EQ);
            poke_x(e, R16, 0);
            // ensure upper bytes zeroed
            a64_ldr_imm64(e, R16, SP, 0);
            a64_mov_reg64(e, R17, 31);
            a64_str_imm64(e, R16, SP, 0);
            break;
        }
        default:
            if (op >= EA_OP_I32_LOAD && op <= EA_OP_I64_LOAD32_U) {
                emit_load(c, in);
                break;
            }
            if (op >= EA_OP_I32_STORE && op <= EA_OP_I64_STORE32) {
                emit_store(c, in);
                break;
            }
            if (!compile_scalar_op(c, in)) {
                char tmp[48];
                snprintf(tmp, sizeof(tmp), "op 0x%x not lowered in v1", op);
                if (!c->failed) jfail(c, tmp);
                return false;
            }
            break;
        }
        pc++;
    }
    if (c->reachable) {
        c->insn_at[n] = e->len;
        emit_return(c);
    }
    return true;
}

// ---------------------------------------------------------------- scalar ops
// true tail call: pop this frame, place the args at [entry-a*16, entry), and
// jump to the callee (JIT) or run the bridge and return to OUR caller (interp).
// validation guarantees callee results == this function's results, so the
// callee's results land exactly where our caller expects them.
static void emit_tail_call(JC *c, bool indirect, EaInstr *in) {
    Em *e = &c->em;
    uint32_t a, r;
    if (!indirect) {
        uint32_t callee = in->imm.u32;
        if (callee >= c->m->n_funcs) { jfail(c, "bad callee"); return; }
        const EaFuncType *t = &c->m->types[c->m->funcs[callee].type_idx].func;
        a = t->n_params; r = t->n_results;
    } else {
        const EaFuncType *t = &c->m->types[in->imm.pair.a].func;
        a = t->n_params; r = t->n_results;
    }
    uint32_t depth = c->depth;
    if (indirect) depth--; // table index popped below
    if (!indirect) {
        uint32_t callee = in->imm.u32;
        uint32_t foff = __builtin_offsetof(EaInstance, funcs);
        uint32_t isz = (uint32_t)sizeof(EaFuncInst);
        a64_ldr_imm64(e, R17, R28, foff);
        uint64_t bo = (uint64_t)callee * isz;
        if (bo < 4096) a64_add_imm64(e, R17, R17, (uint32_t)bo);
        else { a64_mov64_imm(e, R0, bo); a64_add_reg64(e, R17, R17, R0); }
    } else {
        if (c->m->tables[in->imm.pair.b].is64) pop_x(e, R4); // i64 table index
        else pop_w(e, R4);
        a64_mov_reg64(e, R0, R27);
        a64_mov_reg64(e, R1, R28);
        a64_movz32(e, R2, in->imm.pair.b & 0xFFFF);
        if (in->imm.pair.b > 0xFFFF) a64_movk32(e, R2, in->imm.pair.b >> 16);
        a64_movz32(e, R3, in->imm.pair.a & 0xFFFF);
        if (in->imm.pair.a > 0xFFFF) a64_movk32(e, R3, in->imm.pair.a >> 16);
        call_helper(c, (const void *)ea_jit_callee_lookup);
        a64_mov_reg64(e, R17, R0); // fi
    }
    // carry the a args from the operand stack up to [entry-a*16, entry)
    // (= fp + 32 + n_params*16 + below - a*16).  dst is far above src, so an
    // ascending per-slot copy is overlap-safe.  (the operand stack lives below
    // the frame: sp = fp - frame_size - depth*16)
    {
        int32_t below = (c->n_res > c->n_params) ? (int32_t)(c->n_res - c->n_params) * SLOT : 0;
        int32_t dst0 = 32 + (int32_t)c->n_params * SLOT + below - (int32_t)a * SLOT;
        for (uint32_t i = 0; i < a; i++) {
            a64_ldr_imm64(e, R16, SP, (int64_t)i * 16);
            a64_str_imm64(e, R16, FP, dst0 + (int64_t)i * 16);
        }
    }
    // pop this frame: x29/x30 restored, sp = fp + 32
    a64_mov_sp_from(e, FP);
    a64_ldp_post64(e, FP, LR, SP, 32);
    {
        int32_t below = (c->n_res > c->n_params) ? (int32_t)(c->n_res - c->n_params) * SLOT : 0;
        int64_t net = (int64_t)c->n_params * SLOT + below; // land at sum_entry
        if (net >= 0) { if (net) a64_add_imm64(e, SP, SP, net); }
        else if (-net < 4096) a64_sub_imm64(e, SP, SP, (uint32_t)(-net));
        else { a64_mov64_imm(e, R16, (uint64_t)(-net)); a64_add_reg64(e, SP, SP, R16); }
    }
    a64_ldrb_imm32(e, R2, R17, __builtin_offsetof(EaFuncInst, is_jit));
    a64_cmp_imm32(e, R2, 0);
    em_bcond_label(e, 0, CC_EQ);
    uint32_t patch_insn = c->em.len - 1;
    // JIT callee: set its context and tail-jump
    a64_ldr_imm64(e, R0, R17, __builtin_offsetof(EaFuncInst, inst));
    a64_mov_reg64(e, R28, R0);
    load_mem_regs(e);
    a64_ldr_imm64(e, R1, R17, __builtin_offsetof(EaFuncInst, jit_entry));
    a64_br_reg(e, R1);
    uint32_t join = c->em.len;
    c->em.buf[patch_insn] = 0x54000000 | CC_EQ | (((uint32_t)(join - patch_insn) & 0x7FFFF) << 5);
    // interpreted callee: bridge runs it; results land at [entry-r*16, entry);
    // x20 survives the C call (callee-saved) so we can still ret to our caller
    a64_mov_reg64(e, R0, R27);
    a64_mov_reg64(e, R1, R17);
    a64_mov_from_sp(e, R2);
    call_helper(c, (const void *)ea_jit_call_interp);
    if (a > r) a64_add_imm64(e, SP, SP, (a - r) * SLOT);
    else if (r > a) a64_sub_imm64(e, SP, SP, (r - a) * SLOT);
    a64_ret(e);
}

static bool compile_scalar_op(JC *c, EaInstr *in) {
    Em *e = &c->em;
    uint32_t op = in->opcode;
    switch (op) {
    // ---- i32 comparisons
    // ---- i32 comparisons
    // cmp_result_w: flags are already set — branch directly to a following
    // br_if when possible instead of materializing the bool
    case EA_OP_I32_EQZ:
        pop_w(e, R16); a64_cmp_imm32(e, R16, 0); cmp_result_w(c, CC_EQ); return true;
    case EA_OP_I32_EQ: pop_pair_w(c); a64_cmp_reg32(e, R16, R17); cmp_result_w(c, CC_EQ); return true;
    case EA_OP_I32_NE: pop_pair_w(c); a64_cmp_reg32(e, R16, R17); cmp_result_w(c, CC_NE); return true;
    case EA_OP_I32_LT_S: pop_pair_w(c); a64_cmp_reg32(e, R16, R17); cmp_result_w(c, CC_LT); return true;
    case EA_OP_I32_LT_U: pop_pair_w(c); a64_cmp_reg32(e, R16, R17); cmp_result_w(c, CC_LO); return true;
    case EA_OP_I32_GT_S: pop_pair_w(c); a64_cmp_reg32(e, R17, R16); cmp_result_w(c, CC_LT); return true;
    case EA_OP_I32_GT_U: pop_pair_w(c); a64_cmp_reg32(e, R17, R16); cmp_result_w(c, CC_LO); return true;
    case EA_OP_I32_LE_S: pop_pair_w(c); a64_cmp_reg32(e, R16, R17); cmp_result_w(c, CC_LE); return true;
    case EA_OP_I32_LE_U: pop_pair_w(c); a64_cmp_reg32(e, R16, R17); cmp_result_w(c, CC_LS); return true;
    case EA_OP_I32_GE_S: pop_pair_w(c); a64_cmp_reg32(e, R16, R17); cmp_result_w(c, CC_GE); return true;
    case EA_OP_I32_GE_U: pop_pair_w(c); a64_cmp_reg32(e, R16, R17); cmp_result_w(c, CC_HS); return true;
    // ---- i64 comparisons (result i32)
    case EA_OP_I64_EQZ:
        pop_x(e, R16); a64_cmp_reg64(e, R16, 31); cmp_result_w(c, CC_EQ); return true;
    case EA_OP_I64_EQ: pop_pair_x(c); a64_cmp_reg64(e, R16, R17); cmp_result_w(c, CC_EQ); return true;
    case EA_OP_I64_NE: pop_pair_x(c); a64_cmp_reg64(e, R16, R17); cmp_result_w(c, CC_NE); return true;
    case EA_OP_I64_LT_S: pop_pair_x(c); a64_cmp_reg64(e, R16, R17); cmp_result_w(c, CC_LT); return true;
    case EA_OP_I64_LT_U: pop_pair_x(c); a64_cmp_reg64(e, R16, R17); cmp_result_w(c, CC_LO); return true;
    case EA_OP_I64_GT_S: pop_pair_x(c); a64_cmp_reg64(e, R17, R16); cmp_result_w(c, CC_LT); return true;
    case EA_OP_I64_GT_U: pop_pair_x(c); a64_cmp_reg64(e, R17, R16); cmp_result_w(c, CC_LO); return true;
    case EA_OP_I64_LE_S: pop_pair_x(c); a64_cmp_reg64(e, R16, R17); cmp_result_w(c, CC_LE); return true;
    case EA_OP_I64_LE_U: pop_pair_x(c); a64_cmp_reg64(e, R16, R17); cmp_result_w(c, CC_LS); return true;
    case EA_OP_I64_GE_S: pop_pair_x(c); a64_cmp_reg64(e, R16, R17); cmp_result_w(c, CC_GE); return true;
    case EA_OP_I64_GE_U: pop_pair_x(c); a64_cmp_reg64(e, R16, R17); cmp_result_w(c, CC_HS); return true;
    // ---- f32/f64 comparisons
    case EA_OP_F32_EQ: emit_fcmp32(c, CC_EQ, false); return true;
    case EA_OP_F32_NE: emit_fcmp32(c, CC_NE, true); return true;
    case EA_OP_F32_LT: emit_fcmp32(c, CC_LT, false); return true;
    case EA_OP_F32_GT: emit_fcmp32(c, CC_GT, false); return true;
    case EA_OP_F32_LE: emit_fcmp32(c, CC_LE, false); return true;
    case EA_OP_F32_GE: emit_fcmp32(c, CC_GE, false); return true;
    case EA_OP_F64_EQ: emit_fcmp64(c, CC_EQ); return true;
    case EA_OP_F64_NE: emit_fcmp64(c, CC_NE); return true;
    case EA_OP_F64_LT: emit_fcmp64(c, CC_LT); return true;
    case EA_OP_F64_GT: emit_fcmp64(c, CC_GT); return true;
    case EA_OP_F64_LE: emit_fcmp64(c, CC_LE); return true;
    case EA_OP_F64_GE: emit_fcmp64(c, CC_GE); return true;
    // ---- i32 arithmetic
    case EA_OP_I32_ADD: pop_pair_w(c); a64_add_reg32(e, R16, R16, R17); push_result_w(c); return true;
    case EA_OP_I32_SUB: pop_pair_w(c); a64_sub_reg32(e, R16, R16, R17); push_result_w(c); return true;
    case EA_OP_I32_MUL: pop_pair_w(c); a64_madd32(e, R16, R16, R17, 31); push_result_w(c); return true;
    case EA_OP_I32_AND: pop_pair_w(c); a64_and_reg32(e, R16, R16, R17); push_result_w(c); return true;
    case EA_OP_I32_OR: pop_pair_w(c); a64_orr_reg32(e, R16, R16, R17); push_result_w(c); return true;
    case EA_OP_I32_XOR: pop_pair_w(c); a64_eor_reg32(e, R16, R16, R17); push_result_w(c); return true;
    case EA_OP_I32_SHL: pop_pair_w(c); a64_lslv32(e, R16, R16, R17); push_w(e, R16); return true;
    case EA_OP_I32_SHR_S: pop_pair_w(c); a64_asrv32(e, R16, R16, R17); push_w(e, R16); return true;
    case EA_OP_I32_SHR_U: pop_pair_w(c); a64_lsrv32(e, R16, R16, R17); push_w(e, R16); return true;
    case EA_OP_I32_ROTR: pop_w(e, R17); pop_w(e, R16); a64_rorv32(e, R16, R16, R17); push_w(e, R16); return true;
    case EA_OP_I32_ROTL: {
        pop_w(e, R17); pop_w(e, R16);
        a64_neg32(e, R17, R17);
        a64_rorv32(e, R16, R16, R17);
        push_w(e, R16);
        return true;
    }
    case EA_OP_I32_CLZ: pop_w(e, R16); a64_clz32(e, R16, R16); push_w(e, R16); return true;
    case EA_OP_I32_CTZ: pop_w(e, R16); a64_rbit32(e, R16, R16); a64_clz32(e, R16, R16); push_w(e, R16); return true;
    case EA_OP_I32_POPCNT: pop_w(e, R16); { // bit trick: no single instr
        uint32_t save = e->len;
        (void)save;
        a64_mov_reg32(e, R0, R16);
        call_helper(c, (const void *)ea_h_popcnt64);
        a64_mov_reg32(e, R16, R0);
    } push_w(e, R16); return true;
    // ---- i64 arithmetic
    case EA_OP_I64_ADD: pop_pair_x(c); a64_add_reg64(e, R16, R16, R17); push_result_x(c); return true;
    case EA_OP_I64_SUB: pop_pair_x(c); a64_sub_reg64(e, R16, R16, R17); push_result_x(c); return true;
    case EA_OP_I64_MUL: pop_pair_x(c); a64_madd64(e, R16, R16, R17, 31); push_result_x(c); return true;
    case EA_OP_I64_AND: pop_pair_x(c); a64_and_reg64(e, R16, R16, R17); push_result_x(c); return true;
    case EA_OP_I64_OR: pop_pair_x(c); a64_orr_reg64(e, R16, R16, R17); push_result_x(c); return true;
    case EA_OP_I64_XOR: pop_pair_x(c); a64_eor_reg64(e, R16, R16, R17); push_result_x(c); return true;
    case EA_OP_I64_SHL: pop_pair_x(c); a64_lslv64(e, R16, R16, R17); push_x(e, R16); return true;
    case EA_OP_I64_SHR_S: pop_pair_x(c); a64_asrv64(e, R16, R16, R17); push_x(e, R16); return true;
    case EA_OP_I64_SHR_U: pop_pair_x(c); a64_lsrv64(e, R16, R16, R17); push_x(e, R16); return true;
    case EA_OP_I64_ROTR: pop_x(e, R17); pop_x(e, R16); a64_rorv64(e, R16, R16, R17); push_x(e, R16); return true;
    case EA_OP_I64_ROTL: {
        pop_x(e, R17); pop_x(e, R16);
        a64_neg64(e, R17, R17);
        a64_rorv64(e, R16, R16, R17);
        push_x(e, R16);
        return true;
    }
    case EA_OP_I64_CLZ: pop_x(e, R16); a64_clz64(e, R16, R16); push_x(e, R16); return true;
    case EA_OP_I64_CTZ: pop_x(e, R16); a64_rbit64(e, R16, R16); a64_clz64(e, R16, R16); push_x(e, R16); return true;
    case EA_OP_I64_POPCNT: pop_x(e, R16); a64_mov_reg64(e, R0, R16); call_helper(c, (const void *)ea_h_popcnt64); push_x(e, R0); return true;
    default:
        return compile_scalar_op2(c, in);
    }
    return false;
}

static bool compile_scalar_op2(JC *c, EaInstr *in) {
    Em *e = &c->em;
    uint32_t op = in->opcode;
    switch (op) {
    // ---- constants
    case EA_OP_I32_CONST:
        if (c->def_count == 1 && c->def_first) {
            a64_mov32_imm(e, R17, in->imm.u32);
            c->def_kind1 = 0; c->def_count = 2; c->def_first = 0;
        } else if (c->def_count == 0 && defer1_possible(c, c->cur_pc, c->f->code.n)) {
            a64_mov32_imm(e, R17, in->imm.u32);
            c->def_kind1 = 0; c->def_count = 1;
        } else if (c->def_count == 0 && defer2_possible(c, c->cur_pc, c->f->code.n)) {
            a64_mov32_imm(e, R16, in->imm.u32);
            c->def_kind0 = 0; c->def_count = 1; c->def_first = 1;
        } else {
            a64_mov32_imm(e, R16, in->imm.u32);
            push_w(e, R16);
        }
        return true;
    case EA_OP_I64_CONST:
        if (c->def_count == 1 && c->def_first) {
            a64_mov64_imm(e, R17, in->imm.u64);
            c->def_kind1 = 1; c->def_count = 2; c->def_first = 0;
        } else if (c->def_count == 0 && defer1_possible(c, c->cur_pc, c->f->code.n)) {
            a64_mov64_imm(e, R17, in->imm.u64);
            c->def_kind1 = 1; c->def_count = 1;
        } else if (c->def_count == 0 && defer2_possible(c, c->cur_pc, c->f->code.n)) {
            a64_mov64_imm(e, R16, in->imm.u64);
            c->def_kind0 = 1; c->def_count = 1; c->def_first = 1;
        } else {
            a64_mov64_imm(e, R16, in->imm.u64);
            push_x(e, R16);
        }
        return true;
    case EA_OP_F32_CONST:
        a64_mov32_imm(e, R16, *(const uint32_t *)&in->imm.f32);
        push_w(e, R16);
        return true;
    case EA_OP_F64_CONST:
        a64_mov64_imm(e, R16, *(const uint64_t *)&in->imm.f64);
        push_x(e, R16);
        return true;
    // ---- i32 div/rem
    case EA_OP_I32_DIV_S: emit_div32(c, true, false); return true;
    case EA_OP_I32_DIV_U: emit_div32(c, false, false); return true;
    case EA_OP_I32_REM_S: emit_div32(c, true, true); return true;
    case EA_OP_I32_REM_U: emit_div32(c, false, true); return true;
    case EA_OP_I64_DIV_S: emit_div64(c, true, false); return true;
    case EA_OP_I64_DIV_U: emit_div64(c, false, false); return true;
    case EA_OP_I64_REM_S: emit_div64(c, true, true); return true;
    case EA_OP_I64_REM_U: emit_div64(c, false, true); return true;
    // ---- sign extension / wrap / extend
    case EA_OP_I32_EXTEND8_S: pop_w(e, R16); a64_sxtb32(e, R16, R16); push_w(e, R16); return true;
    case EA_OP_I32_EXTEND16_S: pop_w(e, R16); a64_sxth32(e, R16, R16); push_w(e, R16); return true;
    case EA_OP_I64_EXTEND8_S: pop_w(e, R16); a64_sxtb64(e, R16, R16); push_x(e, R16); return true;
    case EA_OP_I64_EXTEND16_S: pop_w(e, R16); a64_sxth64(e, R16, R16); push_x(e, R16); return true;
    case EA_OP_I64_EXTEND32_S: pop_w(e, R16); a64_sxtw64(e, R16, R16); push_x(e, R16); return true;
    case EA_OP_I32_WRAP_I64: pop_x(e, R16); push_w(e, R16); return true;
    case EA_OP_I64_EXTEND_I32_S: pop_w(e, R16); a64_sxtw64(e, R16, R16); push_x(e, R16); return true;
    case EA_OP_I64_EXTEND_I32_U: pop_w(e, R16); push_x(e, R16); return true;
    // ---- reinterpret: slot-preserving no-op
    case EA_OP_I32_REINTERPRET_F32: case EA_OP_F32_REINTERPRET_I32:
    case EA_OP_I64_REINTERPRET_F64: case EA_OP_F64_REINTERPRET_I64:
        return true;
    // ---- float binary
    case EA_OP_F32_ADD: emit_f32_bin(c, 0x0200); return true;
    case EA_OP_F32_SUB: emit_f32_bin(c, 0x0300); return true;
    case EA_OP_F32_MUL: emit_f32_bin(c, 0x0100); return true;
    case EA_OP_F32_DIV: emit_f32_bin(c, 0x0180); return true;
    case EA_OP_F64_ADD: emit_f64_bin(c, 0x0200); return true;
    case EA_OP_F64_SUB: emit_f64_bin(c, 0x0300); return true;
    case EA_OP_F64_MUL: emit_f64_bin(c, 0x0100); return true;
    case EA_OP_F64_DIV: emit_f64_bin(c, 0x0180); return true;
    case EA_OP_F32_MIN: emit_f32_minmax(c, false); return true;
    case EA_OP_F32_MAX: emit_f32_minmax(c, true); return true;
    case EA_OP_F64_MIN: emit_f64_minmax(c, false); return true;
    case EA_OP_F64_MAX: emit_f64_minmax(c, true); return true;
    // ---- float unary
    case EA_OP_F32_ABS: pop_s(e, V0); a64_fabs(e, V0, V0, 4); push_s(e, V0); return true;
    case EA_OP_F32_NEG: pop_s(e, V0); a64_fneg(e, V0, V0, 4); push_s(e, V0); return true;
    case EA_OP_F32_SQRT: pop_s(e, V0); a64_fsqrt(e, V0, V0, 4); push_s(e, V0); return true;
    case EA_OP_F32_CEIL: pop_s(e, V0); a64_frint(e, V0, V0, 4, A64_FRINTP); push_s(e, V0); return true;
    case EA_OP_F32_FLOOR: pop_s(e, V0); a64_frint(e, V0, V0, 4, A64_FRINTM); push_s(e, V0); return true;
    case EA_OP_F32_TRUNC: pop_s(e, V0); a64_frint(e, V0, V0, 4, A64_FRINTZ); push_s(e, V0); return true;
    case EA_OP_F32_NEAREST: pop_s(e, V0); a64_frint(e, V0, V0, 4, A64_FRINTN); push_s(e, V0); return true;
    case EA_OP_F64_ABS: pop_d(e, V0); a64_fabs(e, V0, V0, 8); push_d(e, V0); return true;
    case EA_OP_F64_NEG: pop_d(e, V0); a64_fneg(e, V0, V0, 8); push_d(e, V0); return true;
    case EA_OP_F64_SQRT: pop_d(e, V0); a64_fsqrt(e, V0, V0, 8); push_d(e, V0); return true;
    case EA_OP_F64_CEIL: pop_d(e, V0); a64_frint(e, V0, V0, 8, A64_FRINTP); push_d(e, V0); return true;
    case EA_OP_F64_FLOOR: pop_d(e, V0); a64_frint(e, V0, V0, 8, A64_FRINTM); push_d(e, V0); return true;
    case EA_OP_F64_TRUNC: pop_d(e, V0); a64_frint(e, V0, V0, 8, A64_FRINTZ); push_d(e, V0); return true;
    case EA_OP_F64_NEAREST: pop_d(e, V0); a64_frint(e, V0, V0, 8, A64_FRINTN); push_d(e, V0); return true;
    case EA_OP_F32_COPYSIGN: {
        pop_x(e, R17); pop_x(e, R16);
        a64_mov32_imm(e, R0, 0x7FFFFFFFu);
        a64_and_reg32(e, R16, R16, R0);
        a64_mov32_imm(e, R0, 0x80000000u);
        a64_and_reg32(e, R17, R17, R0);
        a64_orr_reg32(e, R16, R16, R17);
        push_x(e, R16);
        return true;
    }
    case EA_OP_F64_COPYSIGN: {
        pop_x(e, R17); pop_x(e, R16);
        a64_mov64_imm(e, R0, 0x7FFFFFFFFFFFFFFFull);
        a64_and_reg64(e, R16, R16, R0);
        a64_mov64_imm(e, R0, 0x8000000000000000ull);
        a64_and_reg64(e, R17, R17, R0);
        a64_orr_reg64(e, R16, R16, R17);
        push_x(e, R16);
        return true;
    }
    // ---- conversions
    case EA_OP_F32_CONVERT_I32_S: pop_w(e, R16); a64_scvtf(e, 0, R16, 4, 4); fmov_to_gpr(c, R16, 0, 4); push_x(e, R16); return true;
    case EA_OP_F32_CONVERT_I32_U: pop_w(e, R16); a64_ucvtf(e, 0, R16, 4, 4); fmov_to_gpr(c, R16, 0, 4); push_x(e, R16); return true;
    case EA_OP_F32_CONVERT_I64_S: pop_x(e, R16); a64_scvtf(e, 0, R16, 4, 8); fmov_to_gpr(c, R16, 0, 4); push_x(e, R16); return true;
    case EA_OP_F32_CONVERT_I64_U: pop_x(e, R16); a64_ucvtf(e, 0, R16, 4, 8); fmov_to_gpr(c, R16, 0, 4); push_x(e, R16); return true;
    case EA_OP_F64_CONVERT_I32_S: pop_w(e, R16); a64_scvtf(e, 0, R16, 8, 4); fmov_to_gpr(c, R16, 0, 8); push_x(e, R16); return true;
    case EA_OP_F64_CONVERT_I32_U: pop_w(e, R16); a64_ucvtf(e, 0, R16, 8, 4); fmov_to_gpr(c, R16, 0, 8); push_x(e, R16); return true;
    case EA_OP_F64_CONVERT_I64_S: pop_x(e, R16); a64_scvtf(e, 0, R16, 8, 8); fmov_to_gpr(c, R16, 0, 8); push_x(e, R16); return true;
    case EA_OP_F64_CONVERT_I64_U: pop_x(e, R16); a64_ucvtf(e, 0, R16, 8, 8); fmov_to_gpr(c, R16, 0, 8); push_x(e, R16); return true;
    case EA_OP_F32_DEMOTE_F64: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 8); a64_fcvt_ds(e, 0, 0); fmov_to_gpr(c, R16, 0, 4); push_x(e, R16); return true;
    case EA_OP_F64_PROMOTE_F32: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 4); a64_fcvt_sd(e, 0, 0); fmov_to_gpr(c, R16, 0, 8); push_x(e, R16); return true;
    case EA_OP_I32_TRUNC_SAT_F32_S: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 4); a64_fcvtzs(e, R16, 0, 4, 4); push_w(e, R16); return true;
    case EA_OP_I32_TRUNC_SAT_F32_U: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 4); a64_fcvtzu(e, R16, 0, 4, 4); push_w(e, R16); return true;
    case EA_OP_I32_TRUNC_SAT_F64_S: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 8); a64_fcvtzs(e, R16, 0, 8, 4); push_w(e, R16); return true;
    case EA_OP_I32_TRUNC_SAT_F64_U: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 8); a64_fcvtzu(e, R16, 0, 8, 4); push_w(e, R16); return true;
    case EA_OP_I64_TRUNC_SAT_F32_S: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 4); a64_fcvtzs(e, R16, 0, 4, 8); push_x(e, R16); return true;
    case EA_OP_I64_TRUNC_SAT_F32_U: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 4); a64_fcvtzu(e, R16, 0, 4, 8); push_x(e, R16); return true;
    case EA_OP_I64_TRUNC_SAT_F64_S: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 8); a64_fcvtzs(e, R16, 0, 8, 8); push_x(e, R16); return true;
    case EA_OP_I64_TRUNC_SAT_F64_U: pop_x(e, R16); fmov_to_fpr(c, 0, R16, 8); a64_fcvtzu(e, R16, 0, 8, 8); push_x(e, R16); return true;
    // ---- trapping truncations via helpers
    case EA_OP_I32_TRUNC_F32_S: emit_trunc(c, ea_h_trunc_i32_f32); return true;
    case EA_OP_I32_TRUNC_F32_U: emit_trunc(c, ea_h_trunc_u32_f32); return true;
    case EA_OP_I32_TRUNC_F64_S: emit_trunc(c, ea_h_trunc_i32_f64); return true;
    case EA_OP_I32_TRUNC_F64_U: emit_trunc(c, ea_h_trunc_u32_f64); return true;
    case EA_OP_I64_TRUNC_F32_S: emit_trunc(c, ea_h_trunc_i64_f32); return true;
    case EA_OP_I64_TRUNC_F32_U: emit_trunc(c, ea_h_trunc_u64_f32); return true;
    case EA_OP_I64_TRUNC_F64_S: emit_trunc(c, ea_h_trunc_i64_f64); return true;
    case EA_OP_I64_TRUNC_F64_U: emit_trunc(c, ea_h_trunc_u64_f64); return true;
    default:
        return false;
    }
    return false;
}

// ---- float helpers bound to JC
static void fmov_to_fpr(JC *c, uint32_t vd, uint32_t gpr, int b) { a64_fmov_gpr_fpr(&c->em, vd, gpr, 1, b); }
static void fmov_to_gpr(JC *c, uint32_t gpr, uint32_t vs, int b) { a64_fmov_gpr_fpr(&c->em, gpr, vs, 0, b); }

static void emit_f32_bin(JC *c, uint32_t opc) {
    if (c->def_count == 1 && c->def_kind1 == 2) { // rhs parked in v1
        c->def_count = 0;
        pop_s(&c->em, V0);
    } else {
        pop_s(&c->em, V1); pop_s(&c->em, V0);
    }
    switch (opc) {
    case 0x0200: a64_fadd(&c->em, V0, V0, V1, 4); break;
    case 0x0300: a64_fsub(&c->em, V0, V0, V1, 4); break;
    case 0x0100: a64_fmul(&c->em, V0, V0, V1, 4); break;
    case 0x0180: a64_fdiv(&c->em, V0, V0, V1, 4); break;
    }
    push_result_f(c, 4);
}
static void emit_f64_bin(JC *c, uint32_t opc) {
    if (c->def_count == 1 && c->def_kind1 == 3) { // rhs parked in v1
        c->def_count = 0;
        pop_d(&c->em, V0);
    } else {
        pop_d(&c->em, V1); pop_d(&c->em, V0);
    }
    switch (opc) {
    case 0x0200: a64_fadd(&c->em, V0, V0, V1, 8); break;
    case 0x0300: a64_fsub(&c->em, V0, V0, V1, 8); break;
    case 0x0100: a64_fmul(&c->em, V0, V0, V1, 8); break;
    case 0x0180: a64_fdiv(&c->em, V0, V0, V1, 8); break;
    }
    push_result_f(c, 8);
}

// trapping float->int truncation via helper (NaN/range checks in C)
static int32_t ea_h_trunc_i32_f32(EaExec *ex, uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 2147483648.0f || f < -2147483648.0f) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (int32_t)f;
}
static int32_t ea_h_trunc_i32_f64(EaExec *ex, double f) {
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 2147483648.0 || f <= -2147483649.0) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (int32_t)f;
}
static uint32_t ea_h_trunc_u32_f32(EaExec *ex, uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 4294967296.0f || f <= -1.0f) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (uint32_t)f;
}
static uint32_t ea_h_trunc_u32_f64(EaExec *ex, double f) {
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 4294967296.0 || f <= -1.0) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (uint32_t)f;
}
static int64_t ea_h_trunc_i64_f32(EaExec *ex, uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 9223372036854775808.0f || f < -9223372036854775808.0f) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (int64_t)f;
}
static int64_t ea_h_trunc_i64_f64(EaExec *ex, double f) {
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 9223372036854775808.0 || f < -9223372036854775808.0) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (int64_t)f;
}
static uint64_t ea_h_trunc_u64_f32(EaExec *ex, uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 18446744073709551616.0f || f <= -1.0f) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (uint64_t)f;
}
static uint64_t ea_h_trunc_u64_f64(EaExec *ex, double f) {
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 18446744073709551616.0 || f <= -1.0) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (uint64_t)f;
}

static void emit_trunc(JC *c, const void *helper) {
    // (ex, bits/double in x1) — pass value in x1
    pop_x(&c->em, R1);
    a64_mov_reg64(&c->em, R0, R27);
    call_helper(c, helper);
    push_x(&c->em, R0);
}

// div/rem with wasm trap semantics
static void emit_div32(JC *c, bool signed_div, bool want_rem) {
    Em *e = &c->em;
    pop_w(e, R17); // divisor
    pop_w(e, R16); // dividend
    a64_cmp_imm32(e, R17, 0);
    trap_if(c, TRAP_DIV_BY_ZERO, CC_EQ);
    if (signed_div && !want_rem) {
        // dividend save + INT_MIN / -1 overflow check
        a64_mov_reg32(e, R0, R16);
        a64_mov32_imm(e, R1, 0x80000000u);
        a64_cmp_reg32(e, R16, R1);
        em_bcond_label(e, 0, CC_NE);
        uint32_t j1 = e->len - 1;
        a64_mov32_imm(e, R1, 0xFFFFFFFFu);
        a64_cmp_reg32(e, R17, R1);
        em_bcond_label(e, 0, CC_EQ);
        fix_to_trap(c, TRAP_INT_OVERFLOW);
        uint32_t L1 = e->len;
        e->buf[j1] = 0x54000000 | CC_NE | (((uint32_t)(L1 - j1) & 0x7FFFF) << 5);
        a64_sdiv32(e, R16, R16, R17);
        push_w(e, R16);
    } else if (signed_div) {
        // remainder: INT_MIN % -1 == 0, no overflow trap
        a64_mov_reg32(e, R0, R16);
        a64_sdiv32(e, R16, R16, R17);
        a64_msub32(e, R16, R16, R17, R0);
        push_w(e, R16);
    } else {
        a64_mov_reg32(e, R0, R16);
        a64_udiv32(e, R16, R16, R17);
        if (want_rem) a64_msub32(e, R16, R16, R17, R0);
        push_w(e, R16);
    }
}

static void emit_div64(JC *c, bool signed_div, bool want_rem) {
    Em *e = &c->em;
    pop_x(e, R17);
    pop_x(e, R16);
    a64_cmp_reg64(e, R17, 31);
    trap_if(c, TRAP_DIV_BY_ZERO, CC_EQ);
    if (signed_div && !want_rem) {
        a64_mov_reg64(e, R0, R16);
        a64_mov64_imm(e, R1, 0x8000000000000000ull);
        a64_cmp_reg64(e, R16, R1);
        em_bcond_label(e, 0, CC_NE);
        uint32_t j1 = e->len - 1;
        a64_mov64_imm(e, R1, 0xFFFFFFFFFFFFFFFFull);
        a64_cmp_reg64(e, R17, R1);
        em_bcond_label(e, 0, CC_EQ);
        fix_to_trap(c, TRAP_INT_OVERFLOW);
        uint32_t L1 = e->len;
        e->buf[j1] = 0x54000000 | CC_NE | (((uint32_t)(L1 - j1) & 0x7FFFF) << 5);
        a64_sdiv64(e, R16, R16, R17);
        push_x(e, R16);
    } else if (signed_div) {
        a64_mov_reg64(e, R0, R16);
        a64_sdiv64(e, R16, R16, R17);
        a64_msub64(e, R16, R16, R17, R0);
        push_x(e, R16);
    } else {
        a64_mov_reg64(e, R0, R16);
        a64_udiv64(e, R16, R16, R17);
        if (want_rem) a64_msub64(e, R16, R16, R17, R0);
        push_x(e, R16);
    }
}

// ---------------------------------------------------------------- module compile
static bool compile_one_function(JC *c) {
    if (!compile_function(c)) return false;
    // trap stubs
    uint32_t used[16];
    uint32_t n_used = 0;
    for (uint32_t i = 0; i < c->ntfx; i++) {
        uint32_t code = c->tfx[i].code;
        bool have = false;
        for (uint32_t j = 0; j < n_used; j++) have |= used[j] == code;
        if (!have && n_used < 16) used[n_used++] = code;
    }
    for (uint32_t j = 0; j < n_used; j++) {
        c->trap_stub_at[j] = c->em.len;
        a64_mov_reg64(&c->em, R0, R27);
        a64_movz32(&c->em, R1, used[j] & 0xFFFF);
        if (used[j] > 0xFFFF) a64_movk32(&c->em, R1, used[j] >> 16);
        a64_mov64_imm(&c->em, R16, (uint64_t)ea_jit_trap_now);
        a64_blr(&c->em, R16);
        a64_brk(&c->em, 0);
    }
    if (getenv("EA_WDBG") && c->n_pc <= 400) {
        for (uint32_t i = 0; i <= c->n_pc; i++)
            if (c->insn_at[i] != UINT32_MAX)
                fprintf(stderr, "[A] f%u pc=%u at=%u\n", c->fn_idx, i, c->insn_at[i]);
    }
    // resolve pc fixups
    for (uint32_t i = 0; i < c->nfx; i++) {
        uint32_t at = c->fx[i].at;
        uint32_t target = c->insn_at[c->fx[i].target_pc];
        if (target == UINT32_MAX) return false;
        int64_t off = (int64_t)target - (int64_t)at;
        if (c->fx[i].cond)
            c->em.buf[at] |= ((uint32_t)(off & 0x7FFFF) << 5) | (c->fx[i].ccode & 0xF);
        else
            c->em.buf[at] |= ((uint32_t)off & 0x3FFFFFF);
    }
    // resolve trap fixups
    for (uint32_t i = 0; i < c->ntfx; i++) {
        uint32_t at = c->tfx[i].at;
        uint32_t code = c->tfx[i].code;
        uint32_t target = UINT32_MAX;
        for (uint32_t j = 0; j < n_used; j++)
            if (used[j] == code) target = c->trap_stub_at[j];
        if (target == UINT32_MAX) return false;
        int64_t off = (int64_t)target - (int64_t)at;
        if (c->tfx[i].cond)
            c->em.buf[at] |= ((uint32_t)(off & 0x7FFFF) << 5) | c->tfx[i].ccode;
        else
            c->em.buf[at] |= ((uint32_t)off & 0x3FFFFFF);
    }
    return true;
}

void ea_jit_compile_module(EaModule *m) {
    if (m->feat.exceptions) return;
    for (uint32_t fi = 0; fi < m->n_funcs; fi++) {
        EaFunc *f = &m->funcs[fi];
        if (f->code.n == 0) continue; // imported
        JC c;
        memset(&c, 0, sizeof(c));
        em_init(&c.em);
        c.m = m;
        c.f = f;
        c.fn_idx = fi;
        c.ft = &m->types[f->type_idx].func;
        c.n_locals = f->n_locals;
        c.n_params = c.ft->n_params;
        c.n_res = c.ft->n_results;
        uint64_t fsz = 48 + (uint64_t)c.n_locals * 16 + ((uint64_t)f->max_stack + 8) * 16;
        if (fsz >= (1 << 20)) { em_free(&c.em); continue; }
        c.frame_size = (uint32_t)((fsz + 15) & ~15u);
        if (!compile_one_function(&c) || c.failed) {
            if (getenv("EA_JIT_STATS"))
                fprintf(stderr, "JIT bail func %u (%u instrs): %s\n", fi, f->code.n,
                        c.why[0] ? c.why : "compile error");
            em_free(&c.em);
            free(c.fx);
            free(c.tfx);
            free(c.insn_at);
            free(c.is_target);
            continue;
        }
        if (getenv("EA_JIT_STATS"))
            fprintf(stderr, "JIT ok func %u -> %u insns\n", fi, (uint32_t)c.em.len);
        uint8_t *dst = region_alloc(c.em.len * 4);
        if (!dst) { em_free(&c.em); free(c.fx); free(c.tfx); free(c.insn_at); free(c.is_target); continue; }
            pthread_jit_write_protect_np(0);
        memcpy(dst, c.em.buf, c.em.len * 4);
        if (getenv("EA_JIT_DUMP")) {
            fprintf(stderr, "JIT DUMP func %u (%u insns):\n", fi, c.em.len);
            for (uint32_t k = 0; k < c.em.len; k++)
                fprintf(stderr, "%08x%s", c.em.buf[k], (k % 8 == 7) ? "\n" : " ");
            fprintf(stderr, "\n");
        }
        sys_icache_invalidate(dst, c.em.len * 4);
        jit_wren(1);
        f->jit_code = dst;
        em_free(&c.em);
        free(c.fx);
        free(c.tfx);
        free(c.insn_at);
        free(c.is_target);
    }
}
