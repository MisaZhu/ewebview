// easm aarch64 JIT — runtime layer: C code targeted by emitted machine code.
//
// This file is the lower half of the JIT (see jit_a64.h for the layer
// contract).  Everything here runs *below* generated code: trap raisers,
// bulk-memory/table helpers, float minmax & truncation helpers, the
// exception unwinder, the interpreter bridge and the C->JIT trampolines.
// Nothing in here touches the compiler state (JC) — that lives in
// jit_a64.c.
//
// Register conventions assumed by generated code (see jit_a64.c header):
//   x25 = memory base, x26 = memory limit, x27 = EaExec*, x28 = EaInstance*.
#include "easm.h"
#include "jit_a64.h"
#include <stdio.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>

// ---------------------------------------------------------------- code region
static uint8_t *g_code = NULL;
static size_t g_code_len = 0, g_code_cap = 0;

void jit_wren(int on) {
    pthread_jit_write_protect_np(on ? 0 : 1);
}

uint8_t *region_alloc(size_t bytes) {
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
void ea_jit_trap_now(EaExec *ex, uint32_t code) {
    ea_trap(ex, (EaTrap)code);
}

// returns 0 on success, 1 when the callee left an uncaught exception in
// ex->pending_exn (the JIT call site then runs its own handler search)
uint64_t ea_jit_call_interp(EaExec *ex, EaFuncInst *fi, WVal *args) {
    if (fi->is_host) { // host function: bridge the arg/result buffers directly
        WVal la[16], lr[16];
        uint32_t np = fi->type->n_params < 16 ? fi->type->n_params : 16;
        for (uint32_t k = 0; k < np; k++) la[k] = args[k];
        fi->host_fn(fi->host_user, la, lr);
        uint32_t nr = fi->type->n_results < 16 ? fi->type->n_results : 16;
        for (uint32_t k = 0; k < nr; k++) args[k] = lr[k];
        return 0;
    }
    uint32_t n = fi->type->n_params;
    uint32_t r = fi->type->n_results;
    // JIT stack layout: wasm arg i lives at args[n-1-i]; push in wasm order
    for (uint32_t k = n; k-- > 0;) ex->stack[ex->sp++] = args[k];
    ex->depth++;
    ea_interp_exec_function(ex, fi);
    ex->depth--;
    // the callee could not handle an exception: leave the machine sp alone and
    // signal the caller frame (marker x0 == 1) to run its own handler search
    if (ex->pending_exn) return 1;
    // write results back where a JIT callee would leave them: result i at
    // [entry - (i+1)*16] = args[n-1-i]; r > n extends below the args block,
    // which is free red-zone space
    for (uint32_t i = 0; i < r; i++)
        args[(int32_t)n - 1 - (int32_t)i] = ex->stack[ex->sp - 1 - i];
    ex->sp -= r;
    return 0;
}

uint64_t ea_h_popcnt64(uint64_t x) { return __builtin_popcountll(x); }

// ---------------------------------------------------------------- exception handling
// Handler stack discipline: each JIT frame's try_tables push entries with the
// frame's fp; the throw helpers only match entries of the CURRENT frame and
// leave caller entries alone.  When no entry of this frame matches, generated
// code returns a marker (x0 == 1) to its caller, whose call site resumes the
// search with its own fp — exceptions therefore chain frame by frame, and
// interpreter frames in between keep using their own ctl-stack unwinding.
static EaEhRet eh_dispatch(EaExec *ex, EaExnInst *exn, void *fp) {
    EaEhRet r = {NULL, 0};
    if (getenv("EA_EHDBG"))
        fprintf(stderr, "[EH] dispatch fp=%p top=%u\n", fp, ex->eh_top);
    while (ex->eh_top) {
        EaEhEntry *h = &ex->eh[ex->eh_top - 1];
        if (getenv("EA_EHDBG"))
            fprintf(stderr, "[EH]   entry fp=%p sp0=%p desc=%p\n", h->fp, (void *)h->sp0, (void *)h->desc);
        if (h->fp != fp) break; // entries of caller frames stay live
        ex->eh_top--;
        for (uint32_t ci = 0; ci < h->desc->n_clauses; ci++) {
            EaEhClause *cc = &h->desc->c[ci];
            bool match = false;
            uint32_t ncarry = 0;
            if (cc->kind <= 1) {
                EaTagInst *want = &h->inst->tags[cc->tag_idx];
                if (exn->tag->ident == want->ident) {
                    match = true;
                    ncarry = exn->n_vals + (cc->kind == 1 ? 1 : 0);
                }
            } else if (cc->kind == 2) {
                match = true;
            } else {
                match = true;
                ncarry = 1;
            }
            if (!match) continue;
            // land exactly where a `br` to the clause's label would: sp at the
            // target label depth, payload (+exnref) pushed on top of it.
            // sp_f[0] is the TOP slot = the LAST payload value (the operand
            // stack grows down), matching the interpreter's S[h+q] = vals[q]
            WVal *sp_t = h->sp0 + cc->delta_up;
            WVal *sp_f = sp_t - ncarry;
            // sp_f[0] is the TOP slot: the exnref (if any) sits on top of the
            // payload, matching the interpreter's S[h+q] = vals[q] layout
            for (uint32_t q = 0; q < exn->n_vals; q++)
                sp_f[ncarry - 1 - q] = exn->vals[q];
            if (cc->want_ref) sp_f[0].ref = exn;
            ex->pending_exn = NULL;
            r.sp = sp_f;
            r.target = cc->target;
            if (getenv("EA_EHDBG"))
                fprintf(stderr, "[EH] catch kind=%u sp=%p target=%p\n", cc->kind,
                        (void *)sp_f, cc->target);
            return r;
        }
    }
    ex->pending_exn = exn;
    if (getenv("EA_EHDBG")) fprintf(stderr, "[EH] propagate exn=%p fp=%p\n", (void *)exn, fp);
    return r;
}

void ea_jit_eh_push(EaExec *ex, EaInstance *inst, EaEhDesc *desc, WVal *sp0, void *fp) {
    if (ex->eh_top == ex->eh_cap) {
        uint32_t cap = ex->eh_cap ? ex->eh_cap * 2 : 32;
        EaEhEntry *eh = (EaEhEntry *)realloc(ex->eh, cap * sizeof(EaEhEntry));
        if (!eh) ea_trap(ex, TRAP_STACK_EXHAUSTED);
        ex->eh = eh;
        ex->eh_cap = cap;
    }
    EaEhEntry *h = &ex->eh[ex->eh_top++];
    h->sp0 = sp0;
    h->fp = fp;
    h->inst = inst;
    h->desc = desc;
    if (getenv("EA_EHDBG"))
        fprintf(stderr, "[EH] push fp=%p sp0=%p desc=%p nc=%u\n", fp, (void *)sp0,
                (void *)desc, desc ? desc->n_clauses : 0);
}

void ea_jit_eh_pop(EaExec *ex) {
    if (ex->eh_top) ex->eh_top--;
}

void ea_jit_eh_popn(EaExec *ex, uint32_t n) {
    if (n > ex->eh_top) n = ex->eh_top;
    ex->eh_top -= n;
}

void ea_jit_eh_pop_frame(EaExec *ex, void *fp) {
    while (ex->eh_top && ex->eh[ex->eh_top - 1].fp == fp) ex->eh_top--;
}

EaEhRet ea_jit_eh_throw(EaExec *ex, EaInstance *inst, uint32_t tag_idx, WVal *sp, void *fp) {
    (void)inst;
    if (tag_idx >= inst->n_tags) ea_trap(ex, TRAP_INDIRECT_CALL); // validator prevents this
    EaTagInst *tag = &inst->tags[tag_idx];
    uint32_t n = tag->type->n_params;
    EaExnInst *exn = (EaExnInst *)ea_malloc(sizeof(EaExnInst) +
        (n ? n : 1) * sizeof(WVal));
    exn->tag = tag;
    exn->n_vals = n;
    // payload sits at [sp, sp + n*16); vals[j] is the (n-1-j)-th slot
    for (uint32_t q = 0; q < n; q++) exn->vals[q] = sp[n - 1 - q];
    return eh_dispatch(ex, exn, fp);
}

EaEhRet ea_jit_eh_throw_ref(EaExec *ex, EaInstance *inst, EaExnInst *exn, void *fp) {
    if (!exn) ea_trap(ex, TRAP_NULL_REF);
    return eh_dispatch(ex, exn, fp);
}

EaEhRet ea_jit_eh_resume(EaExec *ex, EaInstance *inst, void *fp) {
    (void)inst;
    EaExnInst *px = ex->pending_exn;
    if (getenv("EA_EHDBG"))
        fprintf(stderr, "[EH] resume fp=%p pending=%p\n", fp, (void *)px);
    if (!px) {
        EaEhRet r = {NULL, 0};
        return r;
    }
    ex->pending_exn = NULL;
    return eh_dispatch(ex, px, fp);
}
void ea_h_spdump(uint64_t tag, uint64_t sp) {
    fprintf(stderr, "[SP] tag=%llu sp=%llu slots:", (unsigned long long)tag, (unsigned long long)sp);
    for (int i = 0; i < 6; i++)
        fprintf(stderr, " %llu", (unsigned long long)((uint64_t *)sp)[i]);
    fprintf(stderr, "\n");
}
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
    if (!fi) {
        ex->trap = TRAP_UNINIT_ELEM;
        snprintf(ex->trap_msg, sizeof(ex->trap_msg), "uninitialized element %u", elem_idx);
        if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1);
        ea_trap(ex, TRAP_UNINIT_ELEM);
    }
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

WVal *ea_h_memory_size(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx) {
    (void)ex;
    EaMemInst *mem = inst->memories[memidx];
    // 0 args, 1 result: the result occupies the slot BELOW the incoming sp
    if (mem->is64) sp[-1].i64 = mem->pages; else sp[-1].i32 = (uint32_t)mem->pages;
    return sp - 1;
}
WVal *ea_h_memory_grow(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx) {
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
WVal *ea_h_memory_fill(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx) {
    EaMemInst *mem = inst->memories[memidx];
    uint64_t n = mem->is64 ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    uint8_t byte = (uint8_t)sp[1].i32;
    uint64_t d = mem->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    if (d > mem->size || n > mem->size - d) ea_trap(ex, TRAP_OOB_MEMORY);
    if (n) memset(mem->base + d, byte, n);
    return sp - 3;
}
WVal *ea_h_memory_copy(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dst, uint32_t src) {
    EaMemInst *md = inst->memories[dst], *ms = inst->memories[src];
    uint64_t n = (md->is64 && ms->is64) ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    uint64_t sv = ms->is64 ? (uint64_t)sp[1].i64 : (uint64_t)(uint32_t)sp[1].i32;
    uint64_t dv = md->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    if (sv > ms->size || n > ms->size - sv) ea_trap(ex, TRAP_OOB_MEMORY);
    if (dv > md->size || n > md->size - dv) ea_trap(ex, TRAP_OOB_MEMORY);
    if (n) memmove(md->base + dv, ms->base + sv, n);
    return sp - 3;
}
WVal *ea_h_memory_init(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx, uint32_t dataidx) {
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
WVal *ea_h_data_drop(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dataidx, uint32_t unused) {
    (void)ex;
    (void)sp;
    (void)unused;
    inst->data_alive[dataidx] = 0;
    return sp;
}
WVal *ea_h_table_get(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    EaTableInst *t = &inst->tables[tidx];
    uint64_t i = t->is64 ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    if (i >= t->size) ea_trap(ex, TRAP_OOB_TABLE);
    sp[0] = t->elems[i];
    return sp;
}
WVal *ea_h_table_set(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    EaTableInst *t = &inst->tables[tidx];
    WVal v = sp[0];
    uint64_t i = t->is64 ? (uint64_t)sp[1].i64 : (uint64_t)(uint32_t)sp[1].i32;
    if (i >= t->size) ea_trap(ex, TRAP_OOB_TABLE);
    t->elems[i] = v;
    return sp - 2;
}
WVal *ea_h_table_size(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    (void)ex;
    EaTableInst *t = &inst->tables[tidx];
    if (t->is64) sp[-1].i64 = t->size; else sp[-1].i32 = (uint32_t)t->size;
    return sp - 1;
}
WVal *ea_h_table_grow(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
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
WVal *ea_h_table_fill(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx) {
    EaTableInst *t = &inst->tables[tidx];
    uint64_t n = t->is64 ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    WVal v = sp[1];
    uint64_t d = t->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    if (d > t->size || n > t->size - d) ea_trap(ex, TRAP_OOB_TABLE);
    for (uint64_t i = 0; i < n; i++) t->elems[d + i] = v;
    return sp - 3;
}
WVal *ea_h_table_copy(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dtn, uint32_t stn) {
    EaTableInst *td = &inst->tables[dtn], *ts = &inst->tables[stn];
    uint64_t n = (td->is64 && ts->is64) ? (uint64_t)sp[0].i64 : (uint64_t)(uint32_t)sp[0].i32;
    uint64_t sv = ts->is64 ? (uint64_t)sp[1].i64 : (uint64_t)(uint32_t)sp[1].i32;
    uint64_t dv = td->is64 ? (uint64_t)sp[2].i64 : (uint64_t)(uint32_t)sp[2].i32;
    if (dv > td->size || n > td->size - dv) ea_trap(ex, TRAP_OOB_TABLE);
    if (sv > ts->size || n > ts->size - sv) ea_trap(ex, TRAP_OOB_TABLE);
    memmove(&td->elems[dv], &ts->elems[sv], (size_t)n * sizeof(WVal));
    return sp - 3;
}
WVal *ea_h_table_init(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx, uint32_t eidx) {
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
WVal *ea_h_elem_drop(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t eidx, uint32_t unused) {
    (void)ex;
    (void)sp;
    (void)unused;
    inst->elem_alive[eidx] = 0;
    return sp;
}
uint32_t ea_h_f32_min(uint32_t ab, uint32_t bb) {
    float a, b, r;
    memcpy(&a, &ab, 4);
    memcpy(&b, &bb, 4);
    if (a != a) return ab | 0x00400000u; // quiet the NaN (spec: arithmetic nan)
    if (b != b) return bb | 0x00400000u;
    if (a == 0.0f && b == 0.0f) {
        uint32_t ua, ub, rr;
        memcpy(&ua, &ab, 4);
        memcpy(&ub, &bb, 4);
        rr = ((ua | ub) >> 31) ? 0x80000000u : 0; // min(+0,-0) = -0
        return rr;
    }
    r = a < b ? a : b;
    return *(uint32_t *)&r;
}
uint32_t ea_h_f32_max(uint32_t ab, uint32_t bb) {
    float a, b, r;
    memcpy(&a, &ab, 4);
    memcpy(&b, &bb, 4);
    if (a != a) return ab | 0x00400000u; // quiet the NaN (spec: arithmetic nan)
    if (b != b) return bb | 0x00400000u;
    if (a == 0.0f && b == 0.0f) {
        uint32_t ua, ub, rr;
        memcpy(&ua, &ab, 4);
        memcpy(&ub, &bb, 4);
        rr = ((ua & ub) >> 31) ? 0x80000000u : 0; // max(+0,-0) = +0 unless both -0
        return rr;
    }
    r = a > b ? a : b;
    return *(uint32_t *)&r;
}
uint64_t ea_h_f64_min(uint64_t ab, uint64_t bb) {
    double a, b, r;
    memcpy(&a, &ab, 8);
    memcpy(&b, &bb, 8);
    if (a != a) return ab | 0x0008000000000000ull; // quiet the NaN
    if (b != b) return bb | 0x0008000000000000ull;
    if (a == 0.0 && b == 0.0) {
        uint64_t ua, ub, rr;
        memcpy(&ua, &ab, 8);
        memcpy(&ub, &bb, 8);
        rr = ((ua | ub) >> 63) ? 0x8000000000000000ull : 0; // min(+0,-0) = -0
        return rr;
    }
    r = a < b ? a : b;
    return *(uint64_t *)&r;
}
uint64_t ea_h_f64_max(uint64_t ab, uint64_t bb) {
    double a, b, r;
    memcpy(&a, &ab, 8);
    memcpy(&b, &bb, 8);
    if (a != a) return ab | 0x0008000000000000ull; // quiet the NaN
    if (b != b) return bb | 0x0008000000000000ull;
    if (a == 0.0 && b == 0.0) {
        uint64_t ua, ub, rr;
        memcpy(&ua, &ab, 8);
        memcpy(&ub, &bb, 8);
        rr = ((ua & ub) >> 63) ? 0x8000000000000000ull : 0; // max(+0,-0) = +0 unless both -0
        return rr;
    }
    r = a > b ? a : b;
    return *(uint64_t *)&r;
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
    // exception marker: the callee threw and found no handler in its frame;
    // ex->pending_exn carries the exception for the interpreter's ctl unwinder
    if (ex->pending_exn) return 1;
    for (uint32_t i = 0; i < nr; i++) ex->stack[ex->sp++] = buf[i];
    return 0;
}
// ---- float truncation helpers (fused wasm trap semantics)
int32_t ea_h_trunc_i32_f32(EaExec *ex, uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 2147483648.0f || f < -2147483648.0f) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (int32_t)f;
}
int32_t ea_h_trunc_i32_f64(EaExec *ex, double f) {
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 2147483648.0 || f <= -2147483649.0) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (int32_t)f;
}
uint32_t ea_h_trunc_u32_f32(EaExec *ex, uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 4294967296.0f || f <= -1.0f) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (uint32_t)f;
}
uint32_t ea_h_trunc_u32_f64(EaExec *ex, double f) {
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 4294967296.0 || f <= -1.0) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (uint32_t)f;
}
int64_t ea_h_trunc_i64_f32(EaExec *ex, uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 9223372036854775808.0f || f < -9223372036854775808.0f) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (int64_t)f;
}
int64_t ea_h_trunc_i64_f64(EaExec *ex, double f) {
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 9223372036854775808.0 || f < -9223372036854775808.0) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (int64_t)f;
}
uint64_t ea_h_trunc_u64_f32(EaExec *ex, uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 18446744073709551616.0f || f <= -1.0f) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (uint64_t)f;
}
uint64_t ea_h_trunc_u64_f64(EaExec *ex, double f) {
    if (f != f) ea_trap(ex, TRAP_INVALID_CONV);
    if (f >= 18446744073709551616.0 || f <= -1.0) ea_trap(ex, TRAP_INT_OVERFLOW);
    return (uint64_t)f;
}

// ---- trace probes (EA_JIT_TRACE)
void ea_h_trace(uint64_t marker) { fprintf(stderr, "JIT-TRACE marker=%llx\n", (unsigned long long)marker); }

void ea_h_trace2(uint64_t marker, uint64_t sp) {
    fprintf(stderr, "JIT-TRACE2 marker=%llx sp=%llx\n", (unsigned long long)marker, (unsigned long long)sp);
}
void ea_h_trace3(uint64_t marker, uint64_t slot) {
    fprintf(stderr, "JIT-TRACE3 marker=%llx result=%llx\n", (unsigned long long)marker, (unsigned long long)slot);
}
