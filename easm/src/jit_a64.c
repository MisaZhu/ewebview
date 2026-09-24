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
#include <setjmp.h>
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

// returns 0 on success, 1 when the callee left an uncaught exception in
// ex->pending_exn (the JIT call site then runs its own handler search)
static uint64_t ea_jit_call_interp(EaExec *ex, EaFuncInst *fi, WVal *args) {
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

static WVal *ea_h_memory_size(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx) {
    (void)ex;
    EaMemInst *mem = inst->memories[memidx];
    // 0 args, 1 result: the result occupies the slot BELOW the incoming sp
    if (mem->is64) sp[-1].i64 = mem->pages; else sp[-1].i32 = (uint32_t)mem->pages;
    return sp - 1;
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
static WVal *ea_h_data_drop(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dataidx, uint32_t unused) {
    (void)ex;
    (void)sp;
    (void)unused;
    inst->data_alive[dataidx] = 0;
    return sp;
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
    if (t->is64) sp[-1].i64 = t->size; else sp[-1].i32 = (uint32_t)t->size;
    return sp - 1;
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
static WVal *ea_h_elem_drop(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t eidx, uint32_t unused) {
    (void)ex;
    (void)sp;
    (void)unused;
    inst->elem_alive[eidx] = 0;
    return sp;
}
static uint32_t ea_h_f32_min(uint32_t ab, uint32_t bb) {
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
static uint32_t ea_h_f32_max(uint32_t ab, uint32_t bb) {
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
static uint64_t ea_h_f64_min(uint64_t ab, uint64_t bb) {
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
static uint64_t ea_h_f64_max(uint64_t ab, uint64_t bb) {
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
    // exception marker: the callee threw and found no handler in its frame;
    // ex->pending_exn carries the exception for the interpreter's ctl unwinder
    if (ex->pending_exn) return 1;
    for (uint32_t i = 0; i < nr; i++) ex->stack[ex->sp++] = buf[i];
    return 0;
}

// ---------------------------------------------------------------- codegen
#define SLOT 16

// float values move directly through S/D registers (no gpr<->fpr round-trip)
#define V0 0
#define V1 1
#define V16 16
#define V17 17
#define V18 18
#define V2 2
#define V3 3
#define V4 4
#define V5 5
#define V6 6
#define V7 7
#define V8 8
#define V9 9

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
        uint8_t is_try;
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
    bool def2_live;         // deepest pending value parked in x19 (int only)
    uint8_t def2_kind;      // spill width for x19: 0 = i32, 1 = 8-byte
    uint8_t def_first_reg;  // pair-first value lives here (0 = x16); cache
                            // hit parking skips the x16 copy entirely
    uint8_t def_second_reg; // pair-second value lives here (0 = x17)
    uint32_t emit_a, emit_b; // operand registers chosen by pop_pair (16/17
                            // unless the pair-first stayed in its cache reg)
    bool in_place;          // binop computed straight into a cache register
    int8_t in_place_slot;   // that register's cache slot
    bool in_park;           // binop result emitted straight into x17 (the
                            // park register) — no park mov needed
    bool def_b_imm;         // the window's b operand is an unmaterialized
    uint32_t def_imm;       // immediate (the completer skipped its mov)
    bool pop_imm_ok;        // the popped b operand was such an immediate
    uint32_t pop_imm;
    uint8_t n_cref;         // cache-ref operand stack: logical entries just
    uint8_t cref_local[4];  // below the scratch window whose value already
                            // lives in a cache register (zero-code pushes)
    int16_t cache_map[6];   // local cached in x19-x24 per slot (-1 = none)
    uint8_t cache_dirty;    // bit per slot: register fresher than frame
    uint8_t cache_clock;    // eviction hand
    uint8_t cache_on;       // EA_CACHE/EA_WARM opt-in; spec-clean since the
                            // fused-set/tee cache-bypass and pass-2 want
                            // write-back fixes, still gated (no measured
                            // workload win yet; def2 parks x19)
    // warm-loop two-pass state: pass 1 records the cache state at the loop
    // back-edge; the body is then recompiled with those locals pre-loaded at
    // the head, so loop-carried values never touch memory
    uint32_t wl_pass;       // 0 none, 1 record, 2 warm
    uint32_t fn_idx;        // function being compiled (debug)
    uint32_t wl_head_pc, wl_end_idx, wl_loop_pc;
    uint32_t wl_em0, wl_nfx0, wl_ntfx0;
    int16_t wl_want[6];
    uint32_t wl_n_want;
    // exception handling: shared cold stubs (emitted before the body, so all
    // references from the body are backward and need no fixups)
    uint8_t eh_calls;          // call sites must check the exception marker
    uint8_t has_throw, has_throwref;
    uint8_t is_leaf; // no calls/tail calls/EH: prologue stack check skippable
    uint32_t throw_stub_at, throwref_stub_at, eh_resume_at, eh_prop_at, eh_ret_at;
    EaEhDesc **eh_descs;       // descs of this function (patched at publish)
    uint32_t n_eh_descs, cap_eh_descs;
    struct EhFx { EaEhDesc *desc; uint32_t clause; uint32_t tpc; uint8_t func_level; } *eh_fx;
    uint32_t n_eh_fx, cap_eh_fx;
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
    c->fx[c->nfx].cond = false; // realloc'd slots are garbage: must be explicit
    c->fx[c->nfx].ccode = 0;
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

static void spill_crefs(JC *c);
static uint32_t cref_reg(JC *c, uint32_t i);
// a physical push materializes pending refs first: a deeper virtual value
// can only land ABOVE entries that already exist on the stack, so it must
// be written before the push takes the lowest slot
static void push_x(JC *c, uint32_t rt) { if (c->n_cref) spill_crefs(c); a64_str_pre64(&c->em, rt, SP, -16); }
static void pop_x(JC *c, uint32_t rt) {
    if (c->n_cref) { a64_mov_reg64(&c->em, rt, cref_reg(c, --c->n_cref)); return; }
    a64_ldr_post64(&c->em, rt, SP, 16);
}
static void push_w(JC *c, uint32_t rt) { if (c->n_cref) spill_crefs(c); a64_str_pre32(&c->em, rt, SP, -16); }
static void pop_w(JC *c, uint32_t rt) {
    if (c->n_cref) { a64_mov_reg64(&c->em, rt, cref_reg(c, --c->n_cref)); return; }
    a64_ldr_post32(&c->em, rt, SP, 16);
}
static void push_q(JC *c, uint32_t rt) { if (c->n_cref) spill_crefs(c); a64_str_q_pre(&c->em, rt); }
static void pop_q(JC *c, uint32_t rt) { a64_ldr_q_post(&c->em, rt); }
// Q load/store at a signed FP-relative offset (imm9 form, with far fallback
// through the R0 scratch for the deep-local frames)
static void str_q_frame(JC *c, uint32_t rt, int32_t off) {
    if (off >= -256 && off <= 255) {
        em_word(&c->em, 0x3C800000u | (((uint32_t)off & 0x1FF) << 12) | (FP << 5) | rt);
    } else if (off >= 0 && off <= 32760) {
        em_word(&c->em, 0x3D800000u | (((uint32_t)off / 16) << 10) | (FP << 5) | rt);
    } else {
        if (off >= 0 && off <= 4095) a64_add_imm64(&c->em, R0, FP, (uint32_t)off);
        else { a64_mov64_imm(&c->em, R0, (uint64_t)off); a64_add_reg64(&c->em, R0, FP, R0); }
        a64_str_q_reg(&c->em, rt, R0, 0);
    }
}
static void ldr_q_frame(JC *c, uint32_t rt, int32_t off) {
    if (off >= -256 && off <= 255) {
        em_word(&c->em, 0x3CC00000u | (((uint32_t)off & 0x1FF) << 12) | (FP << 5) | rt);
    } else if (off >= 0 && off <= 32760) {
        em_word(&c->em, 0x3DC00000u | (((uint32_t)off / 16) << 10) | (FP << 5) | rt);
    } else {
        if (off >= 0 && off <= 4095) a64_add_imm64(&c->em, R0, FP, (uint32_t)off);
        else { a64_mov64_imm(&c->em, R0, (uint64_t)off); a64_add_reg64(&c->em, R0, FP, R0); }
        a64_ldr_q_reg(&c->em, rt, R0, 0);
    }
}
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
    // a parked producer emits no code, so any branch target whose emit
    // position co-locates with the consumer would skip the park and read a
    // stale register: no target may sit at the producer or right before it
    if (c->is_target[pc] || (pc > 0 && c->is_target[pc - 1])) return false;
    return pc + 1 < n && !c->is_target[pc + 1] && def_consumes(c->f->code.v[pc + 1].opcode);
}
static bool defer2_possible(JC *c, uint32_t pc, uint32_t n) {
    if (c->is_target[pc] || (pc > 0 && c->is_target[pc - 1])) return false;
    return pc + 2 < n && !c->is_target[pc + 1] && !c->is_target[pc + 2] &&
           is_def_producer(c->f->code.v[pc + 1].opcode) &&
           def_consumes(c->f->code.v[pc + 2].opcode);
}
// def2 (x19) parking: [binop][producer][producer][int binop] — three values
// live across the middle producers.  The binop result parks in x19 (deepest),
// the two producers form the register pair above it.  Only pop_pair (int
// binops) knows how to consume x19, so the consumer is restricted to them;
// loads/stores/floats keep their existing flush-based paths.
static bool defer3_possible(JC *c, uint32_t pc, uint32_t n) {
    if (c->is_target[pc] || (pc > 0 && c->is_target[pc - 1])) return false;
    return pc + 3 < n && !c->is_target[pc + 1] && !c->is_target[pc + 2] &&
           !c->is_target[pc + 3] &&
           is_def_producer(c->f->code.v[pc + 1].opcode) &&
           is_def_producer(c->f->code.v[pc + 2].opcode) &&
           int_consumes(c->f->code.v[pc + 3].opcode);
}
// warm pass 2 only: a cache-resident park carries no window-register
// payload, so the producing pc may itself be a branch target — the back-edge
// restores exactly the cache state the park reads.  The park must hit the
// cache (checked by the caller via local_cached_slot).
static bool defer2_possible_head(JC *c, uint32_t pc, uint32_t n) {
    if (c->wl_pass != 2) return false;
    return pc + 2 < n && !c->is_target[pc + 1] && !c->is_target[pc + 2] &&
           is_def_producer(c->f->code.v[pc + 1].opcode) &&
           def_consumes(c->f->code.v[pc + 2].opcode);
}
// producers allowed to complete a def2 triple: int consts and int local.gets
// (an FP local would park into the int pair and the float consumer paths
// would flush the pair-first register away)
static bool def2_producer_ok(JC *c, EaInstr *in) {
    if (in->opcode == EA_OP_I32_CONST || in->opcode == EA_OP_I64_CONST) return true;
    if (in->opcode != EA_OP_LOCAL_GET || in->imm.u32 >= c->n_locals) return false;
    uint8_t t = c->f->locals[in->imm.u32];
    return t == VT_I32 || t == VT_I64;
}
static bool v128_batch1(JC *c, EaInstr *in);
static int32_t local_off(JC *c, uint32_t k);
static void push_s(JC *c, uint32_t vt);
static void push_d(JC *c, uint32_t vt);

// local cache in x19-x24 (callee-saved, so C helpers keep them); the frame
// slot may be stale.  Within loops the warm-backedge restores the recorded
// head state, so loop-carried locals stay in registers across iterations.
#define EA_CACHE_SLOTS 6
static const uint32_t ea_cache_reg[EA_CACHE_SLOTS] = {19, 20, 21, 22, 23, 24};

static int16_t local_cached_slot(JC *c, uint32_t idx) {
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++)
        if (c->cache_map[i] == (int16_t)idx) return (int16_t)i;
    return -1;
}
// ---- cache-ref operand layer: a local.get of a cached local pushes a
// reference instead of copying the value to the stack; pops read the
// register directly.  The refs occupy no physical slots and sit below the
// scratch window in the logical stack.
static uint32_t cref_reg(JC *c, uint32_t i) {
    return ea_cache_reg[local_cached_slot(c, c->cref_local[i])];
}
static void spill_crefs(JC *c) { // deepest first (array order = highest
    for (uint32_t i = 0; i < c->n_cref; i++) {  // address first); 64-bit
        a64_str_pre64(&c->em, cref_reg(c, i), SP, -16);  // stores (i32 cache
    }                          // values are zero-extended by invariant)
    c->n_cref = 0;
}
static bool cref_conflict_any(JC *c, uint32_t idx) {
    for (uint32_t i = 0; i < c->n_cref; i++)
        if (c->cref_local[i] == idx) return true;
    return false;
}
static void pop_pair_w(JC *c) {
    c->vpops = 0;
    c->pop_imm_ok = false;
    c->emit_a = R16; c->emit_b = R17;
    if (c->def2_live && c->def_count < 2) {
        // deepest operand parked in x19: a = x19, b = parked x17 or the stack
        c->def2_live = false;
        a64_mov_reg64(&c->em, R16, R19);
        if (c->def_count == 1) { c->def_count = 0; c->vpops = 2; return; }
        pop_w(c, R17);
        c->vpops = 2;
        return;
    }
    if (c->def_count == 2) { // a and b in their park registers when cached
        c->def_count = 0; c->vpops = 2;
        c->emit_a = c->def_first_reg ? c->def_first_reg : R16;
        if (c->def_b_imm) { c->pop_imm_ok = true; c->pop_imm = c->def_imm; c->emit_b = R17; }
        else { c->pop_imm_ok = false; c->emit_b = c->def_second_reg ? c->def_second_reg : R17; }
        c->def_first_reg = 0; c->def_second_reg = 0; c->def_b_imm = false;
        return;
    }
    if (c->def_count == 1) { // b in x17; a from a cache ref or the stack
        c->def_count = 0; c->vpops = 1;
        if (c->n_cref) c->emit_a = cref_reg(c, --c->n_cref);
        else pop_w(c, R16);
        return;
    }
    if (c->n_cref) { c->emit_b = cref_reg(c, --c->n_cref); c->vpops = 1; }
    else pop_w(c, R17);
    if (c->n_cref) { c->emit_a = cref_reg(c, --c->n_cref); c->vpops = 2; }
    else pop_w(c, R16);
}
static void pop_pair_x(JC *c) {
    c->vpops = 0;
    c->pop_imm_ok = false;
    c->emit_a = R16; c->emit_b = R17;
    if (c->def2_live && c->def_count < 2) {
        c->def2_live = false;
        a64_mov_reg64(&c->em, R16, R19);
        if (c->def_count == 1) { c->def_count = 0; c->vpops = 2; return; }
        pop_x(c, R17);
        c->vpops = 2;
        return;
    }
    if (c->def_count == 2) {
        c->def_count = 0; c->vpops = 2;
        c->emit_a = c->def_first_reg ? c->def_first_reg : R16;
        if (c->def_b_imm) { c->pop_imm_ok = true; c->pop_imm = c->def_imm; c->emit_b = R17; }
        else { c->pop_imm_ok = false; c->emit_b = c->def_second_reg ? c->def_second_reg : R17; }
        c->def_first_reg = 0; c->def_second_reg = 0; c->def_b_imm = false;
        return;
    }
    if (c->def_count == 1) { // b in x17; a from a cache ref or the stack
        c->def_count = 0; c->vpops = 1;
        if (c->n_cref) c->emit_a = cref_reg(c, --c->n_cref);
        else pop_x(c, R16);
        return;
    }
    if (c->n_cref) { c->emit_b = cref_reg(c, --c->n_cref); c->vpops = 1; }
    else pop_x(c, R17);
    if (c->n_cref) { c->emit_a = cref_reg(c, --c->n_cref); c->vpops = 2; }
    else pop_x(c, R16);
}

static void flush_deferred(JC *c) {
    // crefs first: their insert reserves room for the pushes above them, so
    // the window's pre-decrement spills land below the block (the logical
    // top = the lowest addresses)
    spill_crefs(c);
    if (c->def2_live) { // deepest value first, matching logical stack order
        if (c->def2_kind == 0) a64_str_pre32(&c->em, R19, SP, -16);
        else a64_str_pre64(&c->em, R19, SP, -16);
        c->def2_live = false;
    }
    uint32_t r0 = c->def_first_reg ? c->def_first_reg : R16;
    uint32_t r1 = c->def_second_reg ? c->def_second_reg : R17;
    c->def_first_reg = 0; c->def_second_reg = 0;
    if (!c->def_count) return;
    if (c->def_count == 2 && c->def_b_imm)  // an unmaterialized imm operand
        a64_mov64_imm(&c->em, R17, c->def_imm);
    c->def_b_imm = false;
    if (c->def_count == 2) { // deeper value first; deferred slots are the ones sp points at
        if (c->def_kind0 == 0) a64_str_pre32(&c->em, r0, SP, -16);
        else a64_str_pre64(&c->em, r0, SP, -16);
    }
    if (c->def_kind1 == 0) a64_str_pre32(&c->em, r1, SP, -16);
    else if (c->def_kind1 == 1) a64_str_pre64(&c->em, r1, SP, -16);
    else if (c->def_kind1 == 2) a64_str_pre_fpr(&c->em, V1, SP, -16, 4);
    else a64_str_pre_fpr(&c->em, V1, SP, -16, 8);
    c->def_count = 0;
    c->def_first = 0;
    c->fused_cc = -1;
    c->skip_next = 0;
}
// fetch both operands of a binary op; with deferred values they are already
// in x16 (first operand) / x17 (second), so nothing is emitted

static void flush_cache(JC *c) {
    // the cref spill here is load-bearing: the pre-switch runs before the
    // keep gate, so join points materialize pending refs unconditionally
    spill_crefs(c);
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
        if (c->cache_map[i] >= 0) {
            // warm pass 2: the compile-time dirty bit cannot reflect the
            // runtime state (the back-edge feeds the pinned set through the
            // body every iteration, and interior joins see modified values
            // the linear compile saw as clean), so want slots are written
            // back unconditionally
            bool warm_want = false;
            if (c->wl_pass == 2)
                for (uint32_t j = 0; j < c->wl_n_want; j++)
                    warm_want |= c->wl_want[j] == c->cache_map[i];
            if ((c->cache_dirty & (1u << i)) || warm_want)
                a64_str_imm64(&c->em, ea_cache_reg[i], FP, local_off(c, (uint32_t)c->cache_map[i]));
            c->cache_map[i] = -1;
        }
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
// a binop about to be stored straight back to a cached local computes in
// place: `local.set $k (i64.add ... $k ...)` becomes `add xK, xK, xB` with
// no copy in either direction.  Safe generically because the other operand
// never lives in a cache register (window values use x16/x17).
static uint32_t binop_dst(JC *c, EaInstr *in) {
    c->in_place = false;
    c->in_place_slot = -1;
    c->in_park = false;
    uint32_t nx = c->cur_pc + 1;
    if (nx >= (uint32_t)c->f->code.n || c->is_target[nx]) return R16;
    uint32_t op = c->f->code.v[nx].opcode;
    // the next op consumes this result from the park register: emit into
    // x17 directly and the park branch records instead of copying
    if (int_consumes(op)) { c->in_park = true; return R17; }
    if (!c->cache_on) return R16;
    if (op != EA_OP_LOCAL_SET && op != EA_OP_LOCAL_TEE) return R16;
    uint32_t k = c->f->code.v[nx].imm.u32;
    // refs strictly below the top two survive the pops: writing the dst
    // register would mutate a value they reference
    uint32_t kept = c->def_count == 2 ? c->n_cref :
                    c->def_count == 1 ? (c->n_cref ? c->n_cref - 1 : 0) :
                    (c->n_cref > 2 ? c->n_cref - 2 : 0);
    for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
        if (c->cache_map[i] == (int16_t)k) {
            for (uint32_t r = 0; r < kept; r++)
                if (c->cref_local[r] == k) return R16;
            c->in_place = true;
            c->in_place_slot = (int8_t)i;
            return ea_cache_reg[i];
        }
    }
    return R16;
}
static void in_place_finish(JC *c) {
    if (c->in_place_slot >= 0) c->cache_dirty |= (uint8_t)(1u << c->in_place_slot);
    c->skip_next = 1;
    c->in_place = false;
    c->in_place_slot = -1;
}
// the pair's deep value: when the local already sits in a cache register,
// park it THERE (no copy) — pop_pair hands the register to the consuming op
static void park_pair_first(JC *c, uint32_t idx) {
    if (c->cache_on) {
        for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
            if (c->cache_map[i] == (int16_t)idx) {
                c->def_first_reg = (uint8_t)ea_cache_reg[i];
                c->def_kind0 = 1; c->def_count = 1; c->def_first = 1;
                return;
            }
        }
    }
    load_local_val(c, R16, idx);
    c->def_first_reg = 0;
    c->def_kind0 = 1; c->def_count = 1; c->def_first = 1;
}
// the pair's second (top) value: same cache-resident treatment
static void park_pair_second(JC *c, uint32_t idx) {
    if (c->cache_on) {
        for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
            if (c->cache_map[i] == (int16_t)idx) {
                c->def_second_reg = (uint8_t)ea_cache_reg[i];
                c->def_kind1 = 1; c->def_count = 2; c->def_first = 0;
                return;
            }
        }
    }
    load_local_val(c, R17, idx);
    c->def_second_reg = 0;
    c->def_kind1 = 1; c->def_count = 2; c->def_first = 0;
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
        // want = the body's own local set (first-use order, v128 excluded),
        // NOT the back-edge cache residue: pass 1 thrashes when a pre-loop
        // set or >6 candidates contend for the slots, and recording the
        // survivors would freeze the thrash into pass 2.  Evicted locals
        // live in the frame; the head setup reloads them from there.
        c->wl_n_want = 0;
        for (uint32_t p = c->wl_head_pc; p < c->wl_end_idx && c->wl_n_want < EA_CACHE_SLOTS; p++) {
            uint32_t o = c->f->code.v[p].opcode;
            if (o != EA_OP_LOCAL_GET && o != EA_OP_LOCAL_SET && o != EA_OP_LOCAL_TEE) continue;
            uint32_t idx = c->f->code.v[p].imm.u32;
            if (idx < c->n_locals && c->f->locals[idx] == VT_V128) continue;
            bool have = false;
            for (uint32_t j = 0; j < c->wl_n_want; j++) have |= c->wl_want[j] == (int16_t)idx;
            if (!have) c->wl_want[c->wl_n_want++] = (int16_t)idx;
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
    if (c->def2_live) flush_deferred(c); // the bool would push above x19
    push_w(&c->em, R16);
}
// binop result in R16: fuse with an adjacent consumer instead of a stack
// round-trip — LOCAL_SET stores straight to the local slot, a following
// binary/comparison op takes the value from x17
static void push_result_w(JC *c) {
    uint32_t nx = c->cur_pc + 1;
    if (nx < (uint32_t)c->f->code.n && !c->is_target[nx]) {
        uint32_t nop = c->f->code.v[nx].opcode;
        if (nop == EA_OP_LOCAL_SET || nop == EA_OP_LOCAL_TEE) {
            // the fused set/tee leaves no pushed result: x19 must reach the
            // stack or it would be the unparked logical top afterwards
            if (c->def2_live) flush_deferred(c);
        }
        if (nop == EA_OP_LOCAL_SET || nop == EA_OP_LOCAL_TEE) {
            uint32_t k = c->f->code.v[nx].imm.u32;
            // a pending ref to this local would dangle (write-after-ref),
            // and an allocating store could evict a ref'd slot: spill first
            if (c->n_cref && (cref_conflict_any(c, k) || local_cached_slot(c, k) < 0))
                flush_deferred(c);
        }
        if (nop == EA_OP_LOCAL_SET) {
            if (c->in_place) { in_place_finish(c); return; }
            set_local_val(c, c->f->code.v[nx].imm.u32, R16);
            c->skip_next = 1;
            return;
        }
        if (nop == EA_OP_LOCAL_TEE) {
            if (c->in_place) {
                uint32_t r = ea_cache_reg[c->in_place_slot];
                in_place_finish(c);
                push_x(&c->em, r); // tee keeps the value on the stack too
                return;
            }
            set_local_val(c, c->f->code.v[nx].imm.u32, R16); // cache-aware
            push_x(&c->em, R16);
            c->skip_next = 1; // the tee itself is fully fused
            return;
        }
        if (int_consumes(nop) || load_consumes(nop)) {
            if (!c->in_park) a64_mov_reg64(&c->em, R17, R16);
            c->in_park = false;
            c->def_kind1 = 1; c->def_count = 1; c->def_first = 0;
            return;
        }
        if (c->def_count == 0 && !c->def2_live && !c->cache_on && !c->wl_pass &&
            defer3_possible(c, c->cur_pc, (uint32_t)c->f->code.n)) {
            // three live values: this result parks in x19 (deepest) while the
            // next two producers form the register pair above it
            a64_mov_reg64(&c->em, R19, R16);
            c->def2_kind = 1;
            c->def2_live = true;
            return;
        }
    }
    if (c->def_count == 0 && !c->def2_live && !c->in_place &&
        defer2_possible(c, c->cur_pc, (uint32_t)c->f->code.n)) {
        // [result][producer][consumer]: the result becomes the pair's deep
        // value, already in x16 — zero-cost park
        c->def_kind0 = 1; c->def_count = 1; c->def_first = 1;
        c->def_first_reg = 0;
        return;
    }
    if (c->def2_live) flush_deferred(c); // the result would push above x19
    push_w(&c->em, R16);
}
static void push_result_x(JC *c) {
    uint32_t nx = c->cur_pc + 1;
    if (nx < (uint32_t)c->f->code.n && !c->is_target[nx]) {
        uint32_t nop = c->f->code.v[nx].opcode;
        if (nop == EA_OP_LOCAL_SET || nop == EA_OP_LOCAL_TEE) {
            if (c->def2_live) flush_deferred(c);
        }
        if (nop == EA_OP_LOCAL_SET || nop == EA_OP_LOCAL_TEE) {
            uint32_t k = c->f->code.v[nx].imm.u32;
            // a pending ref to this local would dangle (write-after-ref),
            // and an allocating store could evict a ref'd slot: spill first
            if (c->n_cref && (cref_conflict_any(c, k) || local_cached_slot(c, k) < 0))
                flush_deferred(c);
        }
        if (nop == EA_OP_LOCAL_SET) {
            if (c->in_place) { in_place_finish(c); return; }
            set_local_val(c, c->f->code.v[nx].imm.u32, R16);
            c->skip_next = 1;
            return;
        }
        if (nop == EA_OP_LOCAL_TEE) {
            if (c->in_place) {
                uint32_t r = ea_cache_reg[c->in_place_slot];
                in_place_finish(c);
                push_x(&c->em, r); // tee keeps the value on the stack too
                return;
            }
            set_local_val(c, c->f->code.v[nx].imm.u32, R16); // cache-aware
            push_x(&c->em, R16);
            c->skip_next = 1; // the tee itself is fully fused
            return;
        }
        if (int_consumes(nop) || load_consumes(nop)) {
            if (!c->in_park) a64_mov_reg64(&c->em, R17, R16);
            c->in_park = false;
            c->def_kind1 = 1; c->def_count = 1; c->def_first = 0;
            return;
        }
        if (c->def_count == 0 && !c->def2_live && !c->cache_on && !c->wl_pass &&
            defer3_possible(c, c->cur_pc, (uint32_t)c->f->code.n)) {
            a64_mov_reg64(&c->em, R19, R16);
            c->def2_kind = 1;
            c->def2_live = true;
            return;
        }
    }
    if (c->def_count == 0 && !c->def2_live && !c->in_place &&
        defer2_possible(c, c->cur_pc, (uint32_t)c->f->code.n)) {
        c->def_kind0 = 1; c->def_count = 1; c->def_first = 1;
        c->def_first_reg = 0;
        return;
    }
    if (c->def2_live) flush_deferred(c); // the result would push above x19
    push_x(&c->em, R16);
}
// f32/f64 binop result in v0: park in v1 for a following float op, or store
// straight to a local slot
static void push_result_f(JC *c, int b) {
    uint32_t nx = c->cur_pc + 1;
    if (nx < (uint32_t)c->f->code.n && !c->is_target[nx]) {
        uint32_t nop = c->f->code.v[nx].opcode;
        if (nop == EA_OP_LOCAL_SET) {
            // in place when the local is cache-resident: fmov straight into
            // its register (also keeps the register fresh — never bypass
            // set_local_val with a raw frame store)
            if (c->cache_on) {
                uint32_t k = c->f->code.v[nx].imm.u32;
                for (uint32_t i = 0; i < EA_CACHE_SLOTS; i++) {
                    if (c->cache_map[i] == (int16_t)k) {
                        fmov_to_gpr(c, ea_cache_reg[i], V0, b);
                        c->cache_dirty |= (uint8_t)(1u << i);
                        c->skip_next = 1;
                        return;
                    }
                }
            }
            fmov_to_gpr(c, R16, V0, b);
            set_local_val(c, c->f->code.v[nx].imm.u32, R16);
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
static void push_s(JC *c, uint32_t vt) { if (c->n_cref) spill_crefs(c); a64_str_pre_fpr(&c->em, vt, SP, -16, 4); }
static void pop_s(JC *c, uint32_t vt) { a64_ldr_post_fpr(&c->em, vt, SP, 16, 4); }
static void push_d(JC *c, uint32_t vt) { if (c->n_cref) spill_crefs(c); a64_str_pre_fpr(&c->em, vt, SP, -16, 8); }
static void pop_d(JC *c, uint32_t vt) { a64_ldr_post_fpr(&c->em, vt, SP, 16, 8); }
static void peek_x(JC *c, uint32_t rt, uint32_t slot) { a64_ldr_imm64(&c->em, rt, SP, (int64_t)slot * 16); }
static void peek_q(JC *c, uint32_t rt, uint32_t slot) { a64_ldr_imm64(&c->em, rt, SP, (int64_t)slot * 16); } // Q reg, same encoding
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
    // effective address = 0-extended addr + offset, NO 32-bit wrap: the
    // bounds decision uses the full sum (spec), and out-of-range addresses
    // fault inside the 12 GiB reservation
    if (off) {
        if (off < 4096) a64_add_imm64(&c->em, R16, R16, (uint32_t)off);
        else { a64_mov64_imm(&c->em, R17, off); a64_add_reg64(&c->em, R16, R16, R17); }
    }
    // multi-memory: x25/x26 hold memories[0]; load this memory's regs on
    // demand into the dedicated x14/x15 scratch (untouched by everything else)
    uint32_t mem_base = R25, mem_limit = R26;
    if (in->imm.ma.memidx != 0) {
        a64_ldr_imm64(&c->em, R14, R28, __builtin_offsetof(EaInstance, memories));
        a64_ldr_imm64(&c->em, R14, R14, (int64_t)in->imm.ma.memidx * 8);
        a64_ldr_imm64(&c->em, R14, R14, __builtin_offsetof(EaMemInst, base));
        a64_ldr_imm64(&c->em, R15, R28, __builtin_offsetof(EaInstance, memories));
        a64_ldr_imm64(&c->em, R15, R15, (int64_t)in->imm.ma.memidx * 8);
        a64_ldr_imm64(&c->em, R15, R15, __builtin_offsetof(EaMemInst, size));
        mem_base = R14;
        mem_limit = R15;
    }
    uint64_t nat;
    switch (in->opcode) {
    case EA_OP_I32_LOAD8_S: case EA_OP_I32_LOAD8_U:
    case EA_OP_I64_LOAD8_S: case EA_OP_I64_LOAD8_U: nat = 1; break;
    case EA_OP_I32_LOAD16_S: case EA_OP_I32_LOAD16_U:
    case EA_OP_I64_LOAD16_S: case EA_OP_I64_LOAD16_U: nat = 2; break;
    case EA_OP_I32_LOAD: case EA_OP_F32_LOAD:
    case EA_OP_I64_LOAD32_S: case EA_OP_I64_LOAD32_U: nat = 4; break;
    case EA_OP_V128_LOAD: nat = 16; break;
    case EA_OP_V128_LOAD8_SPLAT: case EA_OP_V128_LOAD16_SPLAT:
    case EA_OP_V128_LOAD32_SPLAT: case EA_OP_V128_LOAD64_SPLAT:
    case EA_OP_V128_LOAD32_ZERO: case EA_OP_V128_LOAD64_ZERO:
        nat = in->opcode == EA_OP_V128_LOAD8_SPLAT ? 1 :
              in->opcode == EA_OP_V128_LOAD16_SPLAT ? 2 :
              in->opcode == EA_OP_V128_LOAD64_SPLAT ||
              in->opcode == EA_OP_V128_LOAD64_ZERO ? 8 : 4;
        break;
    default: nat = 8; break;
    }
    // 32-bit memories: out-of-bounds accesses fault inside the 12 GiB
    // PROT_NONE reservation and the signal handler raises the trap, so no
    // explicit check is needed.  64-bit memories have no such bound.
    if (c->m->memories[in->imm.ma.memidx].is64) {
        a64_sub_imm64(&c->em, R17, mem_limit, (uint32_t)nat);
        a64_cmp_reg64(&c->em, R16, R17);
        trap_if(c, TRAP_OOB_MEMORY, CC_HI);
    }
    if (in->opcode == EA_OP_F32_LOAD || in->opcode == EA_OP_F64_LOAD) {
        int b = in->opcode == EA_OP_F64_LOAD ? 8 : 4;
        a64_ldr_reg_fpr(&c->em, V0, mem_base, R16, b);
        push_result_f(c, b);
        return;
    }
    switch (in->opcode) {
    case EA_OP_I32_LOAD: a64_ldr_reg32(&c->em, R16, mem_base, R16); break;
    case EA_OP_I64_LOAD: a64_ldr_reg64(&c->em, R16, mem_base, R16); break;
    case EA_OP_I32_LOAD8_S: a64_ldrsb_reg32(&c->em, R16, mem_base, R16); break;
    case EA_OP_I32_LOAD8_U: a64_ldrb_reg32(&c->em, R16, mem_base, R16); break;
    case EA_OP_I32_LOAD16_S: a64_ldrsh_reg32(&c->em, R16, mem_base, R16); break;
    case EA_OP_I32_LOAD16_U: a64_ldrh_reg32(&c->em, R16, mem_base, R16); break;
    case EA_OP_I64_LOAD8_S: a64_ldrsb_reg64(&c->em, R16, mem_base, R16); break;
    case EA_OP_I64_LOAD8_U: a64_ldrb_reg32(&c->em, R16, mem_base, R16); break;
    case EA_OP_I64_LOAD16_S: a64_ldrsh_reg64(&c->em, R16, mem_base, R16); break;
    case EA_OP_I64_LOAD16_U: a64_ldrh_reg32(&c->em, R16, mem_base, R16); break;
    case EA_OP_I64_LOAD32_S: em_word(&c->em, 0xB8A06800 | (R16 << 16) | (mem_base << 5) | R16); break;
    case EA_OP_I64_LOAD32_U: a64_ldr_reg32(&c->em, R16, mem_base, R16); break;
    case EA_OP_V128_LOAD:
        a64_ldr_q_reg(&c->em, V16, mem_base, R16);
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD8_SPLAT:
        a64_ldrb_reg32(&c->em, R16, mem_base, R16);
        a64_neon_dup(&c->em, 1, V16, R16);
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD16_SPLAT:
        a64_ldrh_reg32(&c->em, R16, mem_base, R16);
        a64_neon_dup(&c->em, 2, V16, R16);
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD32_SPLAT:
        a64_ldr_reg32(&c->em, R16, mem_base, R16);
        a64_neon_dup(&c->em, 4, V16, R16);
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD64_SPLAT:
        a64_ldr_reg64(&c->em, R16, mem_base, R16);
        a64_neon_dup(&c->em, 8, V16, R16);
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD32_ZERO:
        a64_ldr_reg_fpr(&c->em, V16, mem_base, R16, 4); // zero-extends to Q
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD64_ZERO:
        a64_ldr_reg_fpr(&c->em, V16, mem_base, R16, 8);
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD8X8_S:
        a64_ldr_reg_fpr(&c->em, V16, mem_base, R16, 8);
        a64_neon(&c->em, 0x0F08A610u, V16, V16, 0); // sshll v16.8h, v16.8b, #0
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD8X8_U:
        a64_ldr_reg_fpr(&c->em, V16, mem_base, R16, 8);
        a64_neon(&c->em, 0x2F08A610u, V16, V16, 0); // ushll
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD16X4_S:
        a64_ldr_reg_fpr(&c->em, V16, mem_base, R16, 8);
        a64_neon(&c->em, 0x0F10A610u, V16, V16, 0); // sshll v16.4s, v16.4h, #0
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD16X4_U:
        a64_ldr_reg_fpr(&c->em, V16, mem_base, R16, 8);
        a64_neon(&c->em, 0x2F10A610u, V16, V16, 0); // ushll
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD32X2_S:
        a64_ldr_reg_fpr(&c->em, V16, mem_base, R16, 8);
        a64_neon(&c->em, 0x0F20A610u, V16, V16, 0); // sshll v16.2d, v16.2s, #0
        a64_str_q_pre(&c->em, V16);
        return;
    case EA_OP_V128_LOAD32X2_U:
        a64_ldr_reg_fpr(&c->em, V16, mem_base, R16, 8);
        a64_neon(&c->em, 0x2F20A610u, V16, V16, 0); // ushll
        a64_str_q_pre(&c->em, V16);
        return;
    default: a64_ldr_reg64(&c->em, R16, mem_base, R16); break;
    }
    // i64-result loads must push the full 8-byte slot (push_w would leave the
    // upper half stale for any 8-byte consumer, including the return copy)
    if (in->opcode == EA_OP_I64_LOAD ||
        (in->opcode >= EA_OP_I64_LOAD8_S && in->opcode <= EA_OP_I64_LOAD32_U))
        push_result_x(c);
    else
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
        // multi-memory: load this memory's base/limit on demand (x14/x15)
        uint32_t mem_base = R25, mem_limit = R26;
        if (in->imm.ma.memidx != 0) {
            a64_ldr_imm64(&c->em, R14, R28, __builtin_offsetof(EaInstance, memories));
            a64_ldr_imm64(&c->em, R14, R14, (int64_t)in->imm.ma.memidx * 8);
            a64_ldr_imm64(&c->em, R14, R14, __builtin_offsetof(EaMemInst, base));
            a64_ldr_imm64(&c->em, R15, R28, __builtin_offsetof(EaInstance, memories));
            a64_ldr_imm64(&c->em, R15, R15, (int64_t)in->imm.ma.memidx * 8);
            a64_ldr_imm64(&c->em, R15, R15, __builtin_offsetof(EaMemInst, size));
            mem_base = R14;
            mem_limit = R15;
        }
        if (c->m->memories[in->imm.ma.memidx].is64) {
            a64_sub_imm64(&c->em, R17, mem_limit, (uint32_t)b);
            a64_cmp_reg64(&c->em, R16, R17);
            trap_if(c, TRAP_OOB_MEMORY, CC_HI);
        }
        a64_str_reg_fpr(&c->em, vsrc, mem_base, R16, b);
        return;
    }
    if (in->opcode == EA_OP_V128_STORE) pop_q(c, V16); // 16-byte value
    else pop_x(&c->em, R17); // value
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
    case EA_OP_V128_STORE: nat = 16; break;
    default: nat = 8; break;
    }
    // multi-memory: load this memory's base/limit on demand (x14/x15)
    uint32_t mem_base = R25, mem_limit = R26;
    if (in->imm.ma.memidx != 0) {
        a64_ldr_imm64(&c->em, R14, R28, __builtin_offsetof(EaInstance, memories));
        a64_ldr_imm64(&c->em, R14, R14, (int64_t)in->imm.ma.memidx * 8);
        a64_ldr_imm64(&c->em, R14, R14, __builtin_offsetof(EaMemInst, base));
        a64_ldr_imm64(&c->em, R15, R28, __builtin_offsetof(EaInstance, memories));
        a64_ldr_imm64(&c->em, R15, R15, (int64_t)in->imm.ma.memidx * 8);
        a64_ldr_imm64(&c->em, R15, R15, __builtin_offsetof(EaMemInst, size));
        mem_base = R14;
        mem_limit = R15;
    }
    if (c->m->memories[in->imm.ma.memidx].is64) {
        // keep the value; recompute in a free scratch (x0 is free here)
        a64_mov_reg64(&c->em, R0, R17);
        a64_sub_imm64(&c->em, R17, mem_limit, (uint32_t)nat);
        a64_cmp_reg64(&c->em, R16, R17);
        trap_if(c, TRAP_OOB_MEMORY, CC_HI);
        a64_mov_reg64(&c->em, R17, R0);
    }
    switch (in->opcode) {
    case EA_OP_V128_STORE:
        a64_str_q_reg(&c->em, V16, mem_base, R16);
        return;
    case EA_OP_I32_STORE: case EA_OP_F32_STORE: case EA_OP_I64_STORE32:
        a64_str_reg32(&c->em, R17, mem_base, R16);
        break;
    case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: a64_strb_reg32(&c->em, R17, mem_base, R16); break;
    case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: a64_strh_reg32(&c->em, R17, mem_base, R16); break;
    default: a64_str_reg64(&c->em, R17, mem_base, R16); break;
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
        // full 16-byte slot copies: v128 results carry 4 lanes (scalars keep
        // stale upper halves, which their consumers never read)
        int32_t below = (c->n_res > c->n_params) ? (int32_t)(c->n_res - c->n_params) * SLOT : 0;
        for (int32_t i = 0; i < (int32_t)c->n_res; i++) {
            int32_t off = 32 + below + (int32_t)(c->n_params - 1 - i) * 16;
            a64_ldp_off64(&c->em, R16, R17, SP, ((int32_t)c->n_res - 1 - i) * 2);
            // stp imm7 spans only +/-512 bytes; long-argument-list functions
            // need far slots — compute the address into R0 when out of range
            if (off / 8 > 63) {
                if (off <= 4095) a64_add_imm64(&c->em, R0, FP, (uint32_t)off);
                else {
                    a64_mov64_imm(&c->em, R0, (uint64_t)off);
                    a64_add_reg64(&c->em, R0, FP, R0);
                }
                a64_stp_off64(&c->em, R16, R17, R0, 0);
            } else {
                a64_stp_off64(&c->em, R16, R17, FP, off / 8);
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

// ---------------------------------------------------------------- exception codegen
// exception clause descriptors live in the executable region so handler-push
// code can embed their final addresses; carve them from a per-module arena
static uint8_t *g_eh_cursor = NULL, *g_eh_end = NULL;

static EaEhDesc *eh_desc_alloc(JC *c, uint32_t nc) {
    size_t sz = sizeof(EaEhDesc) + (nc ? nc : 1) * sizeof(EaEhClause);
    sz = (sz + 15) & ~(size_t)15;
    if (!g_eh_cursor || g_eh_cursor + sz > g_eh_end) { jfail(c, "eh arena exhausted"); return NULL; }
    EaEhDesc *d = (EaEhDesc *)(void *)g_eh_cursor;
    memset(d, 0, sz);
    g_eh_cursor += sz;
    if (c->n_eh_descs == c->cap_eh_descs) {
        c->cap_eh_descs = c->cap_eh_descs ? c->cap_eh_descs * 2 : 8;
        c->eh_descs = (EaEhDesc **)realloc(c->eh_descs, c->cap_eh_descs * sizeof(*c->eh_descs));
        if (!c->eh_descs) { jfail(c, "oom"); return NULL; }
    }
    c->eh_descs[c->n_eh_descs++] = d;
    return d;
}

static void eh_fixup(JC *c, EaEhDesc *desc, uint32_t clause, uint32_t tpc, uint8_t func_level) {
    if (c->n_eh_fx == c->cap_eh_fx) {
        c->cap_eh_fx = c->cap_eh_fx ? c->cap_eh_fx * 2 : 8;
        c->eh_fx = (struct EhFx *)realloc(c->eh_fx, c->cap_eh_fx * sizeof(*c->eh_fx));
        if (!c->eh_fx) { jfail(c, "oom"); return; }
    }
    c->eh_fx[c->n_eh_fx].desc = desc;
    c->eh_fx[c->n_eh_fx].clause = clause;
    c->eh_fx[c->n_eh_fx].tpc = tpc;
    c->eh_fx[c->n_eh_fx].func_level = func_level;
    c->n_eh_fx++;
}

// plain branch to an already-emitted (backward) stub address
static void eh_b_to(Em *e, uint32_t target_at) {
    int64_t off = (int64_t)target_at - (int64_t)e->len;
    em_word(e, 0x14000000 | ((uint32_t)off & 0x3FFFFFF));
}

// number of live try_table handlers a branch to label l exits: everything
// above the target frame — the target frame's own pop lives at its landing
// point (its end), and function-level labels exit them all
static uint32_t eh_crossings(JC *c, uint32_t l) {
    uint32_t n = 0;
    if (l >= c->csp) {
        for (uint32_t i = 0; i < c->csp; i++) n += c->ctrl[i].is_try;
        return n;
    }
    for (uint32_t i = c->csp - l; i < c->csp; i++) n += c->ctrl[i].is_try;
    return n;
}

static void eh_emit_pops(JC *c, uint32_t n) {
    if (!n) return;
    a64_mov_reg64(&c->em, R0, R27);
    a64_movz32(&c->em, R1, n & 0xFFFF);
    if (n > 0xFFFF) a64_movk32(&c->em, R1, n >> 16);
    call_helper(c, (const void *)ea_jit_eh_popn);
}

// cold stubs shared by every throw/resume site of the function; emitted right
// after the prologue so all references from the body are backward
static void eh_emit_stubs(JC *c) {
    Em *e = &c->em;
    uint32_t prop_at = 0;
    if (c->has_throw) {
        c->throw_stub_at = e->len;
        // reserve scratch below the operand stack: the helper writes the
        // landing payload into [sp_f, sp_t), which lies BELOW the current sp
        // and must not overlap the C helper's own frame
        a64_mov_from_sp(e, R3);          // payload base (w2 = tag, set at site)
        a64_mov_reg64(e, R4, FP);
        a64_sub_imm64(e, SP, SP, 512);   // helper-frame scratch, below the payload
        a64_mov_reg64(e, R0, R27);
        a64_mov_reg64(e, R1, R28);
        call_helper(c, (const void *)ea_jit_eh_throw);
        a64_cmp_imm32(e, R1, 0);
        em_bcond_label(e, 0, CC_EQ);
        uint32_t prop_b = e->len - 1;
        a64_mov_sp_from(e, R0);
        a64_br_reg(e, R1);
        if (!prop_at) prop_at = e->len;
        c->em.buf[prop_b] = 0x54000000 | CC_EQ | (((prop_at - prop_b) & 0x7FFFF) << 5);
        // bare frame restore + marker; sp is left meaningless — every marker
        // path recomputes sp from the handler entry or its own frame
        a64_mov_sp_from(e, FP);
        a64_ldp_post64(e, FP, LR, SP, 32);
        a64_movz32(e, R0, 1);
        a64_ret(e);
        c->eh_prop_at = prop_at;
    }
    if (c->has_throwref) {
        c->throwref_stub_at = e->len;
        a64_sub_imm64(e, SP, SP, 512);   // helper-frame scratch (see the throw stub)
        a64_mov_reg64(e, R0, R27);
        a64_mov_reg64(e, R1, R28);
        a64_mov_reg64(e, R3, FP);        // x2 = exnref, popped at the site
        call_helper(c, (const void *)ea_jit_eh_throw_ref);
        a64_cmp_imm32(e, R1, 0);
        em_bcond_label(e, 0, CC_EQ);
        uint32_t prop_b = e->len - 1;
        a64_mov_sp_from(e, R0);
        a64_br_reg(e, R1);
        if (!prop_at) prop_at = e->len;
        c->em.buf[prop_b] = 0x54000000 | CC_EQ | (((prop_at - prop_b) & 0x7FFFF) << 5);
        a64_mov_sp_from(e, FP);
        a64_ldp_post64(e, FP, LR, SP, 32);
        a64_movz32(e, R0, 1);
        a64_ret(e);
        c->eh_prop_at = prop_at;
    }
    if (c->eh_calls) {
        if (!prop_at) {
            // a function may only propagate (no own throws): the bare return
            // tail is still needed below the resume stub
            prop_at = e->len;
            a64_mov_sp_from(e, FP);
            a64_ldp_post64(e, FP, LR, SP, 32);
            a64_movz32(e, R0, 1);
            a64_ret(e);
        }
        c->eh_prop_at = prop_at;
        c->eh_resume_at = e->len;
        // reserve scratch below the call-site sp: the landing payload lies
        // below it and must not overlap this C helper's frame
        a64_sub_imm64(e, SP, SP, 512);
        a64_mov_reg64(e, R0, R27);
        // this frame's instance was saved below the locals by the prologue
        int32_t so = -32 - (int32_t)c->n_locals * 16 - 16;
        a64_mov64_imm(e, R16, (uint64_t)(int64_t)so);
        a64_add_reg64(e, R28, FP, R16);
        a64_ldr_imm64(e, R28, R28, 0);
        load_mem_regs(e);
        a64_mov_reg64(e, R1, R28);
        a64_mov_reg64(e, R2, FP);
        call_helper(c, (const void *)ea_jit_eh_resume);
        a64_cmp_imm32(e, R1, 0);
        em_bcond_label(e, 0, CC_EQ);
        uint32_t prop_b = e->len - 1;
        c->em.buf[prop_b] = 0x54000000 | CC_EQ | (((prop_at - prop_b) & 0x7FFFF) << 5);
        a64_mov_sp_from(e, R0);
        a64_br_reg(e, R1);
    }
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
        // exception marker from the callee (x0 == 1, pending_exn set)?
        if (c->eh_calls) {
            a64_cmp_imm32(&c->em, R0, 1);
            em_bcond_label(&c->em, 0, CC_EQ);
            uint32_t mb = c->em.len - 1;
            c->em.buf[mb] = 0x54000000 | CC_EQ | (((uint32_t)(c->eh_resume_at - mb) & 0x7FFFF) << 5);
        }
        // callee left sp = entry - R*16 with results at [sp, sp+R*16); skip the bridge
        em_b_label(&c->em, 0);
        uint32_t skip_at = c->em.len - 1;
        uint32_t join = c->em.len;
        c->em.buf[patch_insn] = 0x54000000 | CC_EQ | (((uint32_t)(join - patch_insn) & 0x7FFFF) << 5);
        // interpreted callee path (sp = args_base here).  When the callee
        // returns more results than it took args, reserve the extra result
        // slots below the args block first — the bridge must not write into
        // its own frame (the red zone is NOT free: it holds the saved lr)
        a64_mov_from_sp(&c->em, R2);           // args base
        if (r > a) a64_sub_imm64(&c->em, SP, SP, (r - a) * SLOT);
        a64_mov_reg64(&c->em, R0, R27);
        a64_mov_reg64(&c->em, R1, R17);
        call_helper(c, (const void *)ea_jit_call_interp);
        if (c->eh_calls) {
            a64_cmp_imm32(&c->em, R0, 1);
            em_bcond_label(&c->em, 0, CC_EQ);
            uint32_t mb = c->em.len - 1;
            c->em.buf[mb] = 0x54000000 | CC_EQ | (((uint32_t)(c->eh_resume_at - mb) & 0x7FFFF) << 5);
        }
        if (a > r) a64_add_imm64(&c->em, SP, SP, (a - r) * SLOT);
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
        // JIT path: pop the args into the call transition (entry sp = args_end)
        a64_add_imm64(&c->em, SP, SP, a * SLOT);
        a64_mov_reg64(&c->em, R28, R0);
        load_mem_regs(&c->em);
        a64_blr(&c->em, R1);
        // exception marker from the callee (x0 == 1, pending_exn set)?
        if (c->eh_calls) {
            a64_cmp_imm32(&c->em, R0, 1);
            em_bcond_label(&c->em, 0, CC_EQ);
            uint32_t mb = c->em.len - 1;
            c->em.buf[mb] = 0x54000000 | CC_EQ | (((uint32_t)(c->eh_resume_at - mb) & 0x7FFFF) << 5);
        }
        // callee left sp = entry - R*16 with results at [sp, sp+R*16); skip the bridge
        em_b_label(&c->em, 0);
        uint32_t skip_at = c->em.len - 1;
        uint32_t join = c->em.len;
        c->em.buf[patch_insn] = 0x54000000 | CC_EQ | (((uint32_t)(join - patch_insn) & 0x7FFFF) << 5);
        a64_mov_from_sp(&c->em, R2);           // args base
        if (r > a) a64_sub_imm64(&c->em, SP, SP, (r - a) * SLOT);
        a64_mov_reg64(&c->em, R0, R27);
        a64_mov_reg64(&c->em, R1, R17);
        call_helper(c, (const void *)ea_jit_call_interp);
        if (c->eh_calls) {
            a64_cmp_imm32(&c->em, R0, 1);
            em_bcond_label(&c->em, 0, CC_EQ);
            uint32_t mb = c->em.len - 1;
            c->em.buf[mb] = 0x54000000 | CC_EQ | (((uint32_t)(c->eh_resume_at - mb) & 0x7FFFF) << 5);
        }
        if (a > r) a64_add_imm64(&c->em, SP, SP, (a - r) * SLOT);
        uint32_t join2 = c->em.len;
        c->em.buf[skip_at] = 0x14000000 | ((join2 - skip_at) & 0x3FFFFFF);
        reload_ctx(c);
    }
}

static void emit_fcmp32(JC *c, uint32_t cond, bool use_nan_true) {
    flush_deferred(c);
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
    flush_deferred(c);
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
    // carry the top `arity` slots so they become the top `arity` slots after
    // sp lands at absolute slot `target_depth` ([sp + k*16] is absolute slot
    // (cur - k), so slot s lives at [sp + (cur-s)*16]):
    //   src slot (cur-arity+1+i) -> [sp + (arity-1-i)*16]
    //   dst slot (target_depth-arity+1+i) -> [sp + (up+arity-1-i)*16]
    // ascending i copies high-to-low, which is overlap-safe for the upward
    // shift (up > 0); up == 0 degenerates to a self-copy.  8-byte moves: the
    // carry never changes an operand's type, so a scalar slot's stale upper
    // half is never read by a wider consumer — 16-byte moves would smear it.
    int32_t up = (int32_t)(cur - target_depth); // slots the pointer moves up
    // full 16-byte ldp/stp pairs: a v128 may ride the carried slots, and the
    // type-preserving carry never lets a scalar's stale upper half reach a
    // wider reader (the ascending loop stays overlap-safe for 16-byte moves)
    for (int32_t i = 0; i < (int32_t)arity; i++) {
        a64_ldp_off64(&c->em, R16, R17, SP, (int32_t)(arity - 1 - i) * 2);
        a64_stp_off64(&c->em, R16, R17, SP,
                      (int32_t)((int64_t)up + (int64_t)arity - 1 - i) * 2);
    }
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
    c->def2_live = false;
    c->def_first_reg = 0;
    c->def_second_reg = 0;
    c->n_cref = 0;
    c->in_park = false;
    c->def_b_imm = false;
    c->pop_imm_ok = false;
    c->emit_a = R16; c->emit_b = R17;
    c->in_place = false;
    c->in_place_slot = -1;
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
            if (ti->opcode == EA_OP_BLOCK || ti->opcode == EA_OP_LOOP ||
                ti->opcode == EA_OP_IF || ti->opcode == EA_OP_TRY_TABLE) {
                if (ti->end_idx <= n) c->is_target[ti->end_idx] = 1;
                if (ti->opcode == EA_OP_LOOP && i + 1 <= n) c->is_target[i + 1] = 1;
                if (ti->opcode == EA_OP_IF && ti->else_idx != UINT32_MAX && ti->else_idx + 1 <= n)
                    c->is_target[ti->else_idx + 1] = 1;
            }
        }
    c->is_target[n] = 1;

    Em *e = &c->em;
    // prologue: native stack bound first (deep recursion traps instead of
    // overflowing the C stack), then skip past the incoming args area, and
    // when R > A also reserve slots for the results (they land at
    // [entry_sp-R*16, entry_sp), which would otherwise overlap this frame);
    // then build our frame below it
    {
        // leaf functions cannot recurse and add only their own bounded frame
        // on top of an already-checked caller (invoke bridge or JIT caller),
        // so the per-call native-stack probe is pure overhead for them; keep
        // the check whenever the frame is large against the 256KB margin
        // (never under EA_CACHE/WARM: there the probe's presence is
        // load-bearing — return.wast breaks with it skipped; root cause TBD)
        if (c->cache_on || !c->is_leaf || c->frame_size >= 64 * 1024) {
            // (cmp cannot read sp in the plain register encoding — go via x17)
            a64_mov_from_sp(e, R17);
            a64_ldr_imm64(e, R16, R27, __builtin_offsetof(EaExec, jit_stack_limit));
            a64_cmp_reg64(e, R17, R16);
            trap_if(c, TRAP_STACK_EXHAUSTED, CC_LS);
        }
    }
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
        const EaFuncType *ft = &c->m->types[c->f->type_idx].func;
        bool vzero_emitted = false;
        for (uint32_t i = 0; i < c->n_params && i < c->n_locals; i++) {
            int32_t aoff = 32 + below + (int32_t)(c->n_params - 1 - i) * 16;
            if (ft->params[i] == VT_V128) {
                ldr_q_frame(c, V16, aoff); // v128 params move full 16-byte slots
                str_q_frame(c, V16, local_off(c, i));
            } else {
                a64_ldr_imm64(e, R16, FP, aoff);
                a64_str_imm64(e, R16, FP, local_off(c, i));
            }
        }
        // wasm zeroes declared locals; the frame is raw stack memory
        for (uint32_t i = c->n_params; i < c->n_locals; i++) {
            if (c->f->locals[i] == VT_V128) {
                if (!vzero_emitted) { a64_neon_movi0(e, V16); vzero_emitted = true; }
                str_q_frame(c, V16, local_off(c, i));
            } else {
                a64_str_imm64(e, 31, FP, local_off(c, i));
            }
        }
    }

    // cold exception stubs (throw / throw_ref / resume / propagate) sit before
    // the body so every reference from the body is a backward branch; the
    // entry path branches over them
    uint32_t eh_skip_b = e->len;
    em_word(e, 0x14000000); // b over the stubs (offset patched below)
    eh_emit_stubs(c);
    {
        int64_t off = (int64_t)e->len - (int64_t)eh_skip_b;
        c->em.buf[eh_skip_b] = 0x14000000 | ((uint32_t)off & 0x3FFFFFF);
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
        case EA_OP_TRY_TABLE:
        case EA_OP_ELSE: case EA_OP_END:
            flush_cache(c);
            break;
        }
        c->depth = c->f->depths[pc];
        c->cur_pc = pc;
        EaInstr *in = &code->v[pc];
        uint32_t op = in->opcode;
        if (c->def_count || c->def2_live || c->n_cref) {
            bool keep = c->skip_depth < 0 && c->reachable &&
                        (def_consumes(op) ||
                         (c->cache_on && op == EA_OP_LOCAL_GET && c->skip_depth < 0 &&
                          c->reachable && local_cached_slot(c, in->imm.u32) >= 0) ||
                         (c->def_count == 1 && c->def_first &&
                          is_def_producer(op) &&
                          (defer1_possible(c, pc, n) ||
                           (c->wl_pass == 2 && !c->is_target[pc] &&
                            !c->is_target[pc + 1] && pc + 1 < n &&
                            def_consumes(c->f->code.v[pc + 1].opcode)))) ||
                         (c->def2_live && c->def_count == 0 &&
                          def2_producer_ok(c, in) && defer2_possible(c, pc, n)));
            if (keep && c->def2_live) {
                // x19 may only stay parked inside the four triple shapes —
                // anything else pushes or pops below it while its stack slot
                // is absent.  The parked region is the logical top in every
                // kept shape, so the flush (deepest first) stays position-
                // correct whenever we do flush.
                if (!((c->def_count == 0 && def2_producer_ok(c, in) &&
                       defer2_possible(c, pc, n)) ||
                      (c->def_count == 1 && c->def_first) ||
                      (c->def_count == 1 && !c->def_first && int_consumes(op)) ||
                      (c->def_count == 2 && int_consumes(op))))
                    keep = false;
            }
            if (!keep) flush_deferred(c);
        }
        if (c->skip_depth >= 0) {
            // skipping unreachable code
            if (op == EA_OP_BLOCK || op == EA_OP_LOOP || op == EA_OP_IF ||
                op == EA_OP_TRY_TABLE) {
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
        case EA_OP_UNREACHABLE:
            b_trap(c, TRAP_UNREACHABLE); // must actually trap at runtime
            c->skip_depth = 0;
            c->reachable = false;
            break;
        case EA_OP_BLOCK:
            c->ctrl[c->csp].height = in->height;
            c->ctrl[c->csp].arity = in->arity_out;
            c->ctrl[c->csp].rarity = in->arity_res;
            c->ctrl[c->csp].arity_in = in->arity_in;
            c->ctrl[c->csp].end_idx = in->end_idx;
            c->ctrl[c->csp].else_idx = UINT32_MAX;
            c->ctrl[c->csp].block_idx = pc;
            c->ctrl[c->csp].is_loop = 0;
            c->ctrl[c->csp].is_try = 0;
            c->csp++;
            break;
        case EA_OP_TRY_TABLE: {
            // handler installation: the descriptor captures, per clause, the
            // tag to match and how far the landing sp sits above this
            // try_table's label sp.  Catch labels resolve OUTSIDE the
            // try_table frame (validator: outer_csp); the JIT ctrl stack has
            // no synthetic function frame, so label == outer_csp here means
            // the function-level label (branch to the implicit end).
            uint32_t outer_csp = c->csp;
            EaEhDesc *desc = eh_desc_alloc(c, in->n_catches);
            if (!desc) break;
            desc->n_clauses = in->n_catches;
            for (uint32_t k = 0; k < in->n_catches; k++) {
                EaCatch *cc = &in->catches[k];
                EaEhClause *ec = &desc->c[k];
                ec->kind = cc->kind;
                ec->want_ref = (cc->kind == 1 || cc->kind == 3) ? 1 : 0;
                ec->tag_idx = (cc->kind <= 1) ? cc->tag : UINT32_MAX;
                if (cc->label > outer_csp) { jfail(c, "eh label"); break; }
                if (cc->label == outer_csp) {
                    // function-level label: land on the private return stub
                    ec->height = 0;
                    ec->delta_up = c->depth;
                    eh_fixup(c, desc, k, c->n_pc, 1);
                } else {
                    uint32_t tf = outer_csp - 1 - cc->label;
                    ec->height = c->ctrl[tf].height;
                    ec->delta_up = c->depth - c->ctrl[tf].height;
                    uint32_t tpc = c->ctrl[tf].is_loop ? c->ctrl[tf].block_idx + 1
                                                       : c->ctrl[tf].end_idx;
                    eh_fixup(c, desc, k, tpc, 0);
                }
            }
            if (c->failed) break;
            // interpreter parity: try label arity == result arity
            c->ctrl[c->csp].height = in->height;
            c->ctrl[c->csp].arity = in->arity_res;
            c->ctrl[c->csp].rarity = in->arity_res;
            c->ctrl[c->csp].arity_in = in->arity_in;
            c->ctrl[c->csp].end_idx = in->end_idx;
            c->ctrl[c->csp].else_idx = UINT32_MAX;
            c->ctrl[c->csp].block_idx = pc;
            c->ctrl[c->csp].is_loop = 0;
            c->ctrl[c->csp].is_try = 1;
            c->csp++;
            a64_mov_reg64(e, R0, R27);
            a64_mov_reg64(e, R1, R28);   // inst (tag identity owner)
            a64_mov64_imm(e, R2, (uint64_t)desc);
            a64_mov_from_sp(e, R3);      // sp0 = this try_table's label sp
            a64_mov_reg64(e, R4, FP);
            call_helper(c, (const void *)ea_jit_eh_push);
            break;
        }
        case EA_OP_THROW: {
            // fast path: the innermost enclosing try_table has a single plain
            // catch of this exact tag (or a single catch_all) — drop its
            // handler entry and land directly, exactly like the helper's
            // match would (sp at the label height minus the payload, payload
            // carried as a br), skipping the stub and the dispatch walk
            EaInstr *tin = NULL;
            uint32_t tf = UINT32_MAX;
            for (uint32_t k = c->csp; k-- > 0;) {
                if (c->ctrl[k].is_try) {
                    tf = k;
                    tin = &code->v[c->ctrl[k].block_idx];
                    break;
                }
            }
            if (tin && tin->n_catches == 1 &&
                (tin->catches[0].kind == 2 ||
                 (tin->catches[0].kind == 0 &&
                  tin->catches[0].tag == in->imm.u32))) {
                uint32_t lbl = tin->catches[0].label;
                uint32_t theight, tarity, tpc;
                if (lbl >= tf) { // function-level clause label
                    theight = 0;
                    tarity = c->n_res;
                    tpc = c->n_pc;
                } else {
                    uint32_t f = tf - 1 - lbl;
                    theight = c->ctrl[f].height;
                    tarity = tin->catches[0].kind == 2 ? 0 : c->ctrl[f].arity;
                    tpc = c->ctrl[f].is_loop ? c->ctrl[f].block_idx + 1
                                             : c->ctrl[f].end_idx;
                }
                a64_mov_reg64(e, R0, R27);
                call_helper(c, (const void *)ea_jit_eh_pop); // drop the entry
                emit_br_to(c, c->depth, theight + tarity, tarity);
                em_b_label(e, 0);
                fix_to_pc(c, tpc);
                c->skip_depth = 0;
                c->reachable = false;
                break;
            }
            // payload stays where it is; the helper reads [sp, sp + n*16)
            a64_movz32(e, R2, in->imm.u32 & 0xFFFF);
            if (in->imm.u32 > 0xFFFF) a64_movk32(e, R2, in->imm.u32 >> 16);
            eh_b_to(e, c->throw_stub_at);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        }
        case EA_OP_THROW_REF: {
            pop_x(e, R2);                // exnref
            eh_b_to(e, c->throwref_stub_at);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        }
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
                // normal exit of a try_table: drop its handler.  Branch
                // landings at insn_at[end_idx] (recorded before this) run this
                // pop too — `br` to the try's own label therefore counts only
                // crossings ABOVE this frame, keeping exactly one pop per exit.
                if (c->ctrl[c->csp - 1].is_try) {
                    a64_mov_reg64(e, R0, R27);
                    call_helper(c, (const void *)ea_jit_eh_pop);
                }
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
            eh_emit_pops(c, eh_crossings(c, l));
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
            // pops belong to the TAKEN path only, after the flag-consuming
            // branch (the helper clobbers flags)
            eh_emit_pops(c, eh_crossings(c, l));
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
                eh_emit_pops(c, eh_crossings(c, pool[base + i]));
                emit_br_to(c, c->depth - 1, th + ta, ta);
                em_b_label(e, 0);
                fix_to_pc(c, tpc);
                c->em.buf[at[i]] = 0x54000000 | CC_EQ | (((case_at - at[i]) & 0x7FFFF) << 5);
            }
            {
                uint32_t case_at = c->em.len, th, ta, tpc;
                bool tl;
                br_label_target(c, pool[base + ntbl], &th, &ta, &tl, &tpc);
                eh_emit_pops(c, eh_crossings(c, pool[base + ntbl]));
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
            eh_emit_pops(c, eh_crossings(c, UINT32_MAX));
            emit_return(c);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        case EA_OP_RETURN_CALL:
            eh_emit_pops(c, eh_crossings(c, UINT32_MAX));
            emit_tail_call(c, false, in);
            c->skip_depth = 0;
            c->reachable = false;
            break;
        case EA_OP_RETURN_CALL_INDIRECT:
            eh_emit_pops(c, eh_crossings(c, UINT32_MAX));
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
            // one 64-bit csel covers every select_t payload (int bits, float
            // bit patterns, references)
            a64_csel64(e, R17, R0, R17, CC_NE);
            push_x(e, R17);
            break;
        case EA_OP_LOCAL_GET:
            if (in->imm.u32 < c->n_locals && c->f->locals[in->imm.u32] == VT_V128) {
                flush_deferred(c);
                ldr_q_frame(c, V16, local_off(c, in->imm.u32));
                push_q(c, V16);
                break;
            }
            if (c->def_count == 1 && c->def_first) {
                park_pair_second(c, in->imm.u32);
            } else if (c->def_count == 0 && c->def2_live && def2_producer_ok(c, in) &&
                       defer2_possible(c, pc, n)) {
                park_pair_first(c, in->imm.u32); // middle of a def2 triple
            } else if (c->def_count == 0 && defer1_possible(c, pc, n)) {
                load_local_val(c, R17, in->imm.u32);
                c->def_kind1 = 1; c->def_count = 1;
            } else if (c->def_count == 0 && defer2_possible(c, pc, n)) {
                park_pair_first(c, in->imm.u32);
            } else if (c->def_count == 0 && local_cached_slot(c, in->imm.u32) >= 0 &&
                       defer2_possible_head(c, pc, n)) {
                park_pair_first(c, in->imm.u32); // cache-resident: target-safe
            } else if (c->cache_on && c->n_cref < 4 &&
                       local_cached_slot(c, in->imm.u32) >= 0) {
                // zero-code push: the value already lives in its cache
                // register; safe at any pc (non-targets executed the get,
                // targets have an empty map except the warm head whose
                // back-edge restores exactly this state)
                c->cref_local[c->n_cref++] = (uint8_t)in->imm.u32;
            } else {
                load_local_val(c, R16, in->imm.u32);
                push_x(e, R16);
            }
            break;
        case EA_OP_LOCAL_SET:
            if (in->imm.u32 < c->n_locals && c->f->locals[in->imm.u32] == VT_V128) {
                flush_deferred(c);
                pop_q(c, V16);
                str_q_frame(c, V16, local_off(c, in->imm.u32));
                break;
            }
            pop_x(e, R16);
            set_local_val(c, in->imm.u32, R16);
            break;
        case EA_OP_LOCAL_TEE:
            if (in->imm.u32 < c->n_locals && c->f->locals[in->imm.u32] == VT_V128) {
                flush_deferred(c);
                peek_q(c, V16, 0);
                str_q_frame(c, V16, local_off(c, in->imm.u32));
                break;
            }
            peek_x(e, R16, 0);
            set_local_val(c, in->imm.u32, R16);
            break;
        case EA_OP_GLOBAL_GET:
            a64_ldr_imm64(e, R16, R28, __builtin_offsetof(EaInstance, jit_globals));
            a64_ldr_imm64(e, R16, R16, (int64_t)in->imm.u32 * 8);
            if (in->imm.u32 < c->m->n_globals_def &&
                c->m->globals_def[in->imm.u32].type == VT_V128) {
                // v128 globals occupy full 16-byte slots
                a64_ldp_off64(e, R0, R1, R16, 0);
                a64_stp_pre64(e, R0, R1, SP, -16);
            } else {
                a64_ldr_imm64(e, R16, R16, 0);
                push_x(e, R16);
            }
            break;
        case EA_OP_GLOBAL_SET: {
            bool g128 = in->imm.u32 < c->m->n_globals_def &&
                        c->m->globals_def[in->imm.u32].type == VT_V128;
            if (g128) {
                a64_ldp_post64(e, R16, R17, SP, 16);
                a64_ldr_imm64(e, R0, R28, __builtin_offsetof(EaInstance, jit_globals));
                a64_ldr_imm64(e, R0, R0, (int64_t)in->imm.u32 * 8);
                a64_stp_off64(e, R16, R17, R0, 0);
            } else {
                pop_x(e, R16);
                a64_ldr_imm64(e, R17, R28, __builtin_offsetof(EaInstance, jit_globals));
                a64_ldr_imm64(e, R17, R17, (int64_t)in->imm.u32 * 8);
                a64_str_imm64(e, R16, R17, 0);
            }
            break;
        }
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
            if (v128_batch1(c, in)) break;
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
    // private return sequence for catch clauses that target the function-level
    // label (they land here with the payload as the results)
    if (c->n_eh_fx > 0) {
        c->eh_ret_at = e->len;
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
        // v128 params carry as full 16-byte slots (the tail call's sp lands
        // below this frame, so the copies must finish before the pop)
        const EaFuncType *ct = indirect ? &c->m->types[in->imm.pair.a].func
                                        : &c->m->types[c->m->funcs[in->imm.u32].type_idx].func;
        for (uint32_t i = 0; i < a; i++) {
            if (ct->params[i] == VT_V128) {
                em_word(e, 0x3DC00000u | ((uint32_t)i << 10) | (SP << 5) | V16); // ldr q,[sp,i*16]
                str_q_frame(c, V16, dst0 + (int32_t)i * 16);
            } else {
                a64_ldr_imm64(e, R16, SP, (int64_t)i * 16);
                a64_str_imm64(e, R16, FP, dst0 + (int64_t)i * 16);
            }
        }
    }
    // pop this frame: x29/x30 restored, sp = fp + 32
    a64_mov_sp_from(e, FP);
    a64_ldp_post64(e, FP, LR, SP, 32);
    // keep the caller's return address in x20 (callee-saved, survives the C
    // bridge below — the blr for the bridge would otherwise clobber x30 and
    // the final ret would jump back into this sequence)
    a64_mov_reg64(e, R20, LR);
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
    // JIT callee: set its context and tail-jump (both fields read before
    // load_mem_regs clobbers x17)
    a64_ldr_imm64(e, R0, R17, __builtin_offsetof(EaFuncInst, inst));
    a64_ldr_imm64(e, R1, R17, __builtin_offsetof(EaFuncInst, jit_entry));
    a64_mov_reg64(e, R28, R0);
    load_mem_regs(e);
    a64_br_reg(e, R1);
    uint32_t join = c->em.len;
    c->em.buf[patch_insn] = 0x54000000 | CC_EQ | (((uint32_t)(join - patch_insn) & 0x7FFFF) << 5);
    // interpreted callee: bridge runs it; results land at [entry-r*16, entry);
    // x20 survives the C call (callee-saved) so we can still ret to our caller.
    // The bridge expects sp at the args BASE (call sites keep args pushed),
    // so pop them here first — the sp adjust below then lands the caller at
    // entry - r*16 exactly as a JIT callee would
    a64_sub_imm64(e, SP, SP, a * SLOT);    // sp = args base
    a64_mov_from_sp(e, R2);
    if (r > a) a64_sub_imm64(e, SP, SP, (r - a) * SLOT); // reserve results
    a64_mov_reg64(e, R0, R27);
    a64_mov_reg64(e, R1, R17);
    call_helper(c, (const void *)ea_jit_call_interp);
    if (a > r) a64_add_imm64(e, SP, SP, (a - r) * SLOT);
    a64_mov_reg64(e, LR, R20); // restore the caller's return address
    a64_ret(e);
}

// trunc_sat input: consume a parked float operand from v1 when the deferral
// window left one there, else pop the stack into v0
static uint32_t trunc_sat_src(JC *c, int b) {
    uint8_t want = b == 8 ? 3 : 2;
    if (c->def_count == 1 && c->def_kind1 == want) {
        c->def_count = 0;
        return V1;
    }
    if (c->def_count) flush_deferred(c);
    pop_x(&c->em, R16);
    fmov_to_fpr(c, 0, R16, b);
    return V0;
}

// v128 batch 1 (integer/bitwise): returns false when the opcode is not
// lowered (caller bails to the interpreter).  FP ops defer until their NaN
// canonicalization matches the interpreter byte-for-byte.
// v128.loadN_lane / v128.storeN_lane: the vector is the top operand, then
// the address (shared memory plumbing with the scalar load/store paths)
static bool v128_load_store_lane(JC *c, EaInstr *in) {
    Em *e = &c->em;
    bool is_store = in->opcode >= EA_OP_V128_STORE8_LANE;
    uint32_t se = 1; // lane byte width
    if (in->opcode == EA_OP_V128_LOAD16_LANE || in->opcode == EA_OP_V128_STORE16_LANE) se = 2;
    else if (in->opcode == EA_OP_V128_LOAD32_LANE || in->opcode == EA_OP_V128_STORE32_LANE) se = 4;
    else if (in->opcode == EA_OP_V128_LOAD64_LANE || in->opcode == EA_OP_V128_STORE64_LANE) se = 8;
    pop_q(c, V16); // vector (top)
    if (c->m->memories[in->imm.ma.memidx].is64) pop_x(e, R16);
    else pop_w(e, R16); // address
    uint64_t off = in->imm.ma.offset;
    if (off) {
        if (off < 4096) a64_add_imm64(e, R16, R16, (uint32_t)off);
        else { a64_mov64_imm(e, R0, off); a64_add_reg64(e, R16, R16, R0); }
    }
    uint32_t mem_base = R25, mem_limit = R26;
    if (in->imm.ma.memidx != 0) {
        a64_ldr_imm64(e, R14, R28, __builtin_offsetof(EaInstance, memories));
        a64_ldr_imm64(e, R14, R14, (int64_t)in->imm.ma.memidx * 8);
        a64_ldr_imm64(e, R14, R14, __builtin_offsetof(EaMemInst, base));
        a64_ldr_imm64(e, R15, R28, __builtin_offsetof(EaInstance, memories));
        a64_ldr_imm64(e, R15, R15, (int64_t)in->imm.ma.memidx * 8);
        a64_ldr_imm64(e, R15, R15, __builtin_offsetof(EaMemInst, size));
        mem_base = R14;
        mem_limit = R15;
    }
    if (c->m->memories[in->imm.ma.memidx].is64) {
        a64_sub_imm64(e, R17, mem_limit, se);
        a64_cmp_reg64(e, R16, R17);
        trap_if(c, TRAP_OOB_MEMORY, CC_HI);
    }
    if (!is_store) {
        switch (se) {
        case 1: a64_ldrb_reg32(e, R16, mem_base, R16); break;
        case 2: a64_ldrh_reg32(e, R16, mem_base, R16); break;
        case 4: a64_ldr_reg32(e, R16, mem_base, R16); break;
        default: a64_ldr_reg64(e, R16, mem_base, R16); break;
        }
        a64_neon_ins(e, se, in->lane, V16, R16);
        push_q(c, V16);
    } else {
        switch (se) {
        case 1: a64_neon_umov(e, 1, in->lane, R17, V16); a64_strb_reg32(e, R17, mem_base, R16); break;
        case 2: a64_neon_umov(e, 2, in->lane, R17, V16); a64_strh_reg32(e, R17, mem_base, R16); break;
        case 4: a64_neon_umov(e, 4, in->lane, R17, V16); a64_str_reg32(e, R17, mem_base, R16); break;
        default: a64_neon_umov(e, 8, in->lane, R17, V16); a64_str_reg64(e, R17, mem_base, R16); break;
        }
    }
    return true;
}

static bool v128_batch1(JC *c, EaInstr *in) {
    Em *e = &c->em;
    switch (in->opcode) {
    case EA_OP_V128_CONST: {
        uint64_t lo = 0, hi = 0;
        for (int i = 0; i < 8; i++) lo |= (uint64_t)in->imm.bytes[i] << (8 * i);
        for (int i = 0; i < 8; i++) hi |= (uint64_t)in->imm.bytes[8 + i] << (8 * i);
        if (lo == 0 && hi == 0) {
            a64_neon_movi0(e, V16);
        } else {
            a64_mov64_imm(e, R16, lo);
            a64_mov64_imm(e, R17, hi);
            a64_neon_ins(e, 8, 0, V16, R16);
            a64_neon_ins(e, 8, 1, V16, R17);
        }
        push_q(c, V16);
        return true;
    }
    case EA_OP_V128_NOT:
        pop_q(c, V16);
        a64_neon(e, 0x6E205A10u, V16, V16, 0); // mvn
        push_q(c, V16);
        return true;
    case EA_OP_V128_BITSELECT:
        pop_q(c, V18); pop_q(c, V17); pop_q(c, V16); // c, x, y
        a64_neon(e, 0x6E721E30u, V16, V17, V18);     // bsl: (c&x)|(~c&y)
        push_q(c, V16);
        return true;
    case EA_OP_I8X16_SPLAT: case EA_OP_I16X8_SPLAT: case EA_OP_I32X4_SPLAT:
    case EA_OP_F32X4_SPLAT: case EA_OP_I64X2_SPLAT: case EA_OP_F64X2_SPLAT: {
        static const uint32_t se[6] = {1, 2, 4, 8, 4, 8}; // I8,I16,I32,I64,F32,F64
        uint32_t k = in->opcode - EA_OP_I8X16_SPLAT;
        if (se[k] == 8) pop_x(e, R16); else pop_w(e, R16);
        a64_neon_dup(e, se[k], V16, R16);
        push_q(c, V16);
        return true;
    }
    case EA_OP_I8X16_EXTRACT_LANE_S: case EA_OP_I8X16_EXTRACT_LANE_U:
    case EA_OP_I16X8_EXTRACT_LANE_S: case EA_OP_I16X8_EXTRACT_LANE_U:
    case EA_OP_I32X4_EXTRACT_LANE: case EA_OP_I64X2_EXTRACT_LANE:
    case EA_OP_F32X4_EXTRACT_LANE: case EA_OP_F64X2_EXTRACT_LANE: {
        // lane byte width (the opcode family interleaves replace-lane forms,
        // so a base-subtraction index would be non-contiguous)
        uint32_t se;
        switch (in->opcode) {
        case EA_OP_I8X16_EXTRACT_LANE_S: case EA_OP_I8X16_EXTRACT_LANE_U: se = 1; break;
        case EA_OP_I16X8_EXTRACT_LANE_S: case EA_OP_I16X8_EXTRACT_LANE_U: se = 2; break;
        case EA_OP_I32X4_EXTRACT_LANE: case EA_OP_F32X4_EXTRACT_LANE: se = 4; break;
        default: se = 8; break; // I64X2 / F64X2
        }
        pop_q(c, V16);
        a64_neon_umov(e, se, in->lane, R16, V16);
        if (in->opcode == EA_OP_I8X16_EXTRACT_LANE_S)
            em_word(e, 0x13001E10u); // sxtb w16, w16
        else if (in->opcode == EA_OP_I16X8_EXTRACT_LANE_S)
            em_word(e, 0x13003E10u); // sxth w16, w16
        if (se == 8) push_x(e, R16); else push_w(e, R16);
        return true;
    }
    case EA_OP_I8X16_REPLACE_LANE: case EA_OP_I16X8_REPLACE_LANE:
    case EA_OP_I32X4_REPLACE_LANE: case EA_OP_I64X2_REPLACE_LANE:
    case EA_OP_F32X4_REPLACE_LANE: case EA_OP_F64X2_REPLACE_LANE: {
        uint32_t se;
        switch (in->opcode) {
        case EA_OP_I8X16_REPLACE_LANE: se = 1; break;
        case EA_OP_I16X8_REPLACE_LANE: se = 2; break;
        case EA_OP_I32X4_REPLACE_LANE: case EA_OP_F32X4_REPLACE_LANE: se = 4; break;
        default: se = 8; break; // I64X2 / F64X2
        }
        if (se == 8) pop_x(e, R16); else pop_w(e, R16); // scalar (top)
        pop_q(c, V16);
        a64_neon_ins(e, se, in->lane, V16, R16);
        push_q(c, V16);
        return true;
    }
    case EA_OP_V128_AND: case EA_OP_V128_OR: case EA_OP_V128_XOR:
    case EA_OP_V128_ANDNOT:
    case EA_OP_I8X16_ADD: case EA_OP_I8X16_SUB:
    case EA_OP_I16X8_ADD: case EA_OP_I16X8_SUB: case EA_OP_I16X8_MUL:
    case EA_OP_I32X4_ADD: case EA_OP_I32X4_SUB: case EA_OP_I32X4_MUL:
    case EA_OP_I64X2_ADD: case EA_OP_I64X2_SUB:
    case EA_OP_I8X16_EQ: case EA_OP_I16X8_EQ: case EA_OP_I32X4_EQ:
    case EA_OP_I64X2_EQ: case EA_OP_I8X16_NE: case EA_OP_I16X8_NE:
    case EA_OP_I32X4_NE: case EA_OP_I64X2_NE: {
        // pop b then a, op a = a OP b, push
        static const uint32_t add_w[4]  = {0x4E318610u, 0x4E718610u, 0x4EB18610u, 0x4EF18610u};
        static const uint32_t sub_w[4]  = {0x6E318610u, 0x6E718610u, 0x6EB18610u, 0x6EF18610u};
        static const uint32_t mul_w[2]  = {0x4E719E10u, 0x4EB19E10u}; // h, s
        static const uint32_t eq_w[4]   = {0x4E318E10u, 0x4E718E10u, 0x6EB18E10u, 0x6EF18E10u};
        uint32_t sample = 0;
        switch (in->opcode) {
        case EA_OP_V128_AND:    sample = 0x4E311E10u; break;
        case EA_OP_V128_OR:     sample = 0x4EB11E10u; break;
        case EA_OP_V128_XOR:    sample = 0x6E311E10u; break;
        case EA_OP_V128_ANDNOT: sample = 0x4E711E10u; break; // bic: a & ~b
        case EA_OP_I8X16_ADD: sample = add_w[0]; break;
        case EA_OP_I16X8_ADD: sample = add_w[1]; break;
        case EA_OP_I32X4_ADD: sample = add_w[2]; break;
        case EA_OP_I64X2_ADD: sample = add_w[3]; break;
        case EA_OP_I8X16_SUB: sample = sub_w[0]; break;
        case EA_OP_I16X8_SUB: sample = sub_w[1]; break;
        case EA_OP_I32X4_SUB: sample = sub_w[2]; break;
        case EA_OP_I64X2_SUB: sample = sub_w[3]; break;
        case EA_OP_I16X8_MUL: sample = mul_w[0]; break;
        case EA_OP_I32X4_MUL: sample = mul_w[1]; break;
        case EA_OP_I8X16_EQ: sample = eq_w[0]; break;
        case EA_OP_I16X8_EQ: sample = eq_w[1]; break;
        case EA_OP_I32X4_EQ: sample = eq_w[2]; break;
        case EA_OP_I64X2_EQ: sample = eq_w[3]; break;
        case EA_OP_I8X16_NE: sample = eq_w[0]; break;
        case EA_OP_I16X8_NE: sample = eq_w[1]; break;
        case EA_OP_I32X4_NE: sample = eq_w[2]; break;
        case EA_OP_I64X2_NE: sample = eq_w[3]; break;
        }
        pop_q(c, V17); pop_q(c, V16);
        a64_neon(e, sample, V16, V16, V17);
        if (in->opcode == EA_OP_I8X16_NE || in->opcode == EA_OP_I16X8_NE ||
            in->opcode == EA_OP_I32X4_NE || in->opcode == EA_OP_I64X2_NE)
            a64_neon(e, 0x6E205A10u, V16, V16, 0); // mvn
        push_q(c, V16);
        return true;
    }
    case EA_OP_I8X16_ABS: case EA_OP_I16X8_ABS: case EA_OP_I32X4_ABS:
    case EA_OP_I64X2_ABS: case EA_OP_I8X16_NEG: case EA_OP_I16X8_NEG:
    case EA_OP_I32X4_NEG: case EA_OP_I64X2_NEG:
    case EA_OP_F32X4_ABS: case EA_OP_F64X2_ABS: case EA_OP_F32X4_NEG:
    case EA_OP_F64X2_NEG:
    case EA_OP_I8X16_POPCNT: {
        // 2-reg misc family, calibrated at the widest lane of each group
        static const uint32_t abs_w[4] = {0x4E20BA10u, 0x4E60BA10u, 0x4EA0BA10u, 0x4EE0BA10u};
        static const uint32_t neg_w[4] = {0x6E20BA10u, 0x6E60BA10u, 0x6EA0BA10u, 0x6EE0BA10u};
        pop_q(c, V16);
        uint32_t sample;
        switch (in->opcode) {
        case EA_OP_I8X16_ABS: sample = abs_w[0]; break;
        case EA_OP_I16X8_ABS: sample = abs_w[1]; break;
        case EA_OP_I32X4_ABS: sample = abs_w[2]; break;
        case EA_OP_I64X2_ABS: sample = abs_w[3]; break;
        case EA_OP_I8X16_NEG: sample = neg_w[0]; break;
        case EA_OP_I16X8_NEG: sample = neg_w[1]; break;
        case EA_OP_I32X4_NEG: sample = neg_w[2]; break;
        case EA_OP_I64X2_NEG: sample = neg_w[3]; break;
        case EA_OP_F32X4_ABS: sample = 0x4EA0FA10u; break;
        case EA_OP_F64X2_ABS: sample = 0x4EE0FA10u; break;
        case EA_OP_F32X4_NEG: sample = 0x6EA0FA10u; break;
        case EA_OP_F64X2_NEG: sample = 0x6EE0FA10u; break;
        default: sample = 0x4E205A10u; break; // cnt v16.16b
        }
        a64_neon(e, sample, V16, V16, 0);
        push_q(c, V16);
        return true;
    }
    case EA_OP_V128_ANY_TRUE:
        pop_q(c, V16);
        // reductions carry a fixed 10001 pattern in the Rm field position
        a64_neon(e, 0x6E30AA10u, V16, V16, 17); // umaxv b16
        a64_neon_umov(e, 1, 0, R16, V16);
        a64_cmp_imm32(e, R16, 0);
        a64_cset32(e, R16, CC_NE);
        push_w(e, R16);
        return true;
    case EA_OP_I8X16_ALL_TRUE: case EA_OP_I16X8_ALL_TRUE: case EA_OP_I32X4_ALL_TRUE:
        pop_q(c, V16);
        a64_neon(e, 0x6E31AA10u, V16, V16, 17); // uminv b16 (byte-min != 0
        a64_neon_umov(e, 1, 0, R16, V16);       //  <=> every lane != 0)
        a64_cmp_imm32(e, R16, 0);
        a64_cset32(e, R16, CC_NE);
        push_w(e, R16);
        return true;
    case EA_OP_V128_LOAD8_LANE: case EA_OP_V128_LOAD16_LANE:
    case EA_OP_V128_LOAD32_LANE: case EA_OP_V128_LOAD64_LANE:
    case EA_OP_V128_STORE8_LANE: case EA_OP_V128_STORE16_LANE:
    case EA_OP_V128_STORE32_LANE: case EA_OP_V128_STORE64_LANE:
        return v128_load_store_lane(c, in);
    case EA_OP_I16X8_EXTEND_LOW_I8X16_S: case EA_OP_I16X8_EXTEND_HIGH_I8X16_S:
    case EA_OP_I16X8_EXTEND_LOW_I8X16_U: case EA_OP_I16X8_EXTEND_HIGH_I8X16_U:
    case EA_OP_I32X4_EXTEND_LOW_I16X8_S: case EA_OP_I32X4_EXTEND_HIGH_I16X8_S:
    case EA_OP_I32X4_EXTEND_LOW_I16X8_U: case EA_OP_I32X4_EXTEND_HIGH_I16X8_U:
    case EA_OP_I64X2_EXTEND_LOW_I32X4_S: case EA_OP_I64X2_EXTEND_HIGH_I32X4_S:
    case EA_OP_I64X2_EXTEND_LOW_I32X4_U: case EA_OP_I64X2_EXTEND_HIGH_I32X4_U: {
        // shll/ushll (2) with #0, calibrated per (dest width, signedness, half)
        static const uint32_t ext[12] = {
            0x0F08A610u, 0x4F08A610u, 0x2F08A610u, 0x6F08A610u, // i16x8 <- i8x16 s.lo s.hi u.lo u.hi
            0x0F10A610u, 0x4F10A610u, 0x2F10A610u, 0x6F10A610u, // i32x4 <- i16x8
            0x0F20A610u, 0x4F20A610u, 0x2F20A610u, 0x6F20A610u, // i64x2 <- i32x4
        };
        uint32_t k;
        switch (in->opcode) {
        case EA_OP_I16X8_EXTEND_LOW_I8X16_S: k = 0; break;
        case EA_OP_I16X8_EXTEND_HIGH_I8X16_S: k = 1; break;
        case EA_OP_I16X8_EXTEND_LOW_I8X16_U: k = 2; break;
        case EA_OP_I16X8_EXTEND_HIGH_I8X16_U: k = 3; break;
        case EA_OP_I32X4_EXTEND_LOW_I16X8_S: k = 4; break;
        case EA_OP_I32X4_EXTEND_HIGH_I16X8_S: k = 5; break;
        case EA_OP_I32X4_EXTEND_LOW_I16X8_U: k = 6; break;
        case EA_OP_I32X4_EXTEND_HIGH_I16X8_U: k = 7; break;
        case EA_OP_I64X2_EXTEND_LOW_I32X4_S: k = 8; break;
        case EA_OP_I64X2_EXTEND_HIGH_I32X4_S: k = 9; break;
        case EA_OP_I64X2_EXTEND_LOW_I32X4_U: k = 10; break;
        default: k = 11; break;
        }
        pop_q(c, V16);
        a64_neon(e, ext[k], V16, V16, 0);
        push_q(c, V16);
        return true;
    }
    case EA_OP_I8X16_NARROW_I16X8_S: case EA_OP_I8X16_NARROW_I16X8_U:
    case EA_OP_I16X8_NARROW_I32X4_S: case EA_OP_I16X8_NARROW_I32X4_U: {
        // narrow a into the dest low half, b into the high half:
        // sqxtn/uqxtn per operand, then move b's result into d[1]
        uint32_t narrow_lo = in->opcode == EA_OP_I8X16_NARROW_I16X8_S ? 0x0E214A10u :
                             in->opcode == EA_OP_I8X16_NARROW_I16X8_U ? 0x2E214A10u :
                             in->opcode == EA_OP_I16X8_NARROW_I32X4_S ? 0x0E614A10u : 0x2E614A10u;
        pop_q(c, V17); pop_q(c, V16); // b, a
        a64_neon(e, narrow_lo, V16, V16, 0);      // sat(a) -> dest lanes 0..n-1
        a64_neon(e, narrow_lo, V17, V17, 0);      // sat(b) -> scratch low half
        a64_neon(e, 0x6E180400u, V16, V17, 0);    // ins v16.d[1], v17.d[0]
        push_q(c, V16);
        return true;
    }
    case EA_OP_I8X16_SWIZZLE:
        // NEON tbl with a single table: out-of-range indices yield 0,
        // exactly the wasm swizzle semantics
        pop_q(c, V17); pop_q(c, V16);
        a64_neon(e, 0x4E110210u, V16, V16, V17); // tbl v16, {v16}, v17
        push_q(c, V16);
        return true;
    case EA_OP_I8X16_SHUFFLE: {
        // two-entry table {a, b}: materialize the 16 lane indices, then tbl
        uint64_t lo = 0, hi = 0;
        for (int i = 0; i < 8; i++) lo |= (uint64_t)in->imm.bytes[i] << (8 * i);
        for (int i = 0; i < 8; i++) hi |= (uint64_t)in->imm.bytes[8 + i] << (8 * i);
        pop_q(c, V17); pop_q(c, V16); // b, a
        a64_mov64_imm(e, R16, lo);
        a64_mov64_imm(e, R17, hi);
        a64_neon_ins(e, 8, 0, V18, R16);
        a64_neon_ins(e, 8, 1, V18, R17);
        a64_neon(e, 0x4E122210u, V16, V16, V18); // tbl v16, {v16, v17}, v18
        push_q(c, V16);
        return true;
    }
    case EA_OP_SELECT_T:
        // the vector select always carries an explicit type immediate
        if (in->imm.u32 == VT_V128) {
            pop_w(e, R16);                                 // cond
            a64_neon_dup(&c->em, 4, V18, R16);             // dup v18.4s, w16
            a64_neon(&c->em, 0x4EB28E52u, V18, V18, V18);  // cmtst: mask = cond != 0
            pop_q(c, V17); pop_q(c, V16);                  // b, a
            a64_neon(&c->em, 0x6E711E12u, V18, V16, V17);  // bsl v18 = (m&a)|(~m&b)
            push_q(c, V18);
            return true;
        }
        return false; // scalar select_t: the scalar path
    case EA_OP_F32X4_MIN: case EA_OP_F32X4_MAX:
    case EA_OP_F64X2_MIN: case EA_OP_F64X2_MAX:
        // interpreter-parity sequence: NaN -> canonical, +0/-0 by sign rule,
        // else the IEEE min/max.  (NEON fmin/fmax alone = IEEE minNum, which
        // returns the non-NaN operand and leaves +/-0 unordered)
        {
            bool is64 = in->opcode == EA_OP_F64X2_MIN || in->opcode == EA_OP_F64X2_MAX;
            bool is_max = in->opcode == EA_OP_F32X4_MAX || in->opcode == EA_OP_F64X2_MAX;
            pop_q(c, V17); pop_q(c, V16); // b, a
            // NaN lanes: fcmeq(x, x) is false exactly where x is NaN
            a64_neon(e, is64 ? 0x4E70E610u : 0x4E30E610u, V2, V16, V16);
            a64_neon(e, 0x6E205A10u, V2, V2, 0);           // mvn: NaN lanes of a
            a64_neon(e, is64 ? 0x4E70E610u : 0x4E30E610u, V3, V17, V17);
            a64_neon(e, 0x6E205A10u, V3, V3, 0);           // NaN lanes of b
            a64_neon(e, 0x4EB11E10u, V2, V2, V3);          // orr: either-NaN
            // IEEE min/max (NaN lanes produce garbage, overridden below)
            a64_neon(e, is64 ? (is_max ? 0x4E71F610u : 0x4EF1F610u)
                             : (is_max ? 0x4E31F610u : 0x4EB1F610u), V4, V16, V17);
            // +/-0 fixup: where m == 0, result = (sign(a) OP sign(b)) << 63/31
            a64_neon(e, is64 ? 0x6F00E400u : 0x4F000400u, V5, 0, 0); // movi 0
            a64_neon(e, is64 ? 0x4E70E610u : 0x4E30E610u, V5, V4, V5); // m == 0
            a64_neon(e, is64 ? 0x6F410610u : 0x6F210610u, V6, V16, 0); // a >> 63/31
            a64_neon(e, is64 ? 0x6F410610u : 0x6F210610u, V7, V17, 0); // b >> 63/31
            if (is_max) a64_neon(e, 0x4E311E10u, V6, V6, V7); // and: max(+0,-0)=+0
            else        a64_neon(e, 0x4EB11E10u, V6, V6, V7); // orr: min(+0,-0)=-0
            a64_neon(e, is64 ? 0x4F7F5610u : 0x4F3F5610u, V6, V6, 0); // shl 63/31
            a64_neon(e, 0x6E721E30u, V5, V6, V4);          // bsl v5 = (z&s)|(~z&m)
            // canonical NaN override
            if (is64) {
                a64_mov64_imm(e, R16, 0x7FF8000000000000ull);
                a64_neon_ins(e, 8, 0, V8, R16);
                a64_neon_ins(e, 8, 1, V8, R16);
            } else {
                a64_neon(e, 0x4F0367E0u, V8, 0, 0);        // movi 0x7f<<24
                a64_neon(e, 0x4F064400u, V9, 0, 0);        // movi 0xc0<<16
                a64_neon(e, 0x4EB11E10u, V8, V8, V9);      // 0x7fc00000
            }
            a64_neon(e, 0x6E721E30u, V2, V8, V5);          // bsl v2 = (n&c)|(~n&m)
            push_q(c, V2);
            return true;
        }
    case EA_OP_F32X4_PMIN: case EA_OP_F32X4_PMAX:
    case EA_OP_F64X2_PMIN: case EA_OP_F64X2_PMAX: {
        // wasm pmin/pmax = the compare-select form (NaN -> a, +0-biased):
        //   pmin(a,b) = (b < a) ? b : a   pmax(a,b) = (a < b) ? b : a
        // via fcmgt + bsl (the FP compares are calibrated per width — the
        // FP size encoding differs from the integer families)
        bool is64 = in->opcode == EA_OP_F64X2_PMIN || in->opcode == EA_OP_F64X2_PMAX;
        bool is_max = in->opcode == EA_OP_F32X4_PMAX || in->opcode == EA_OP_F64X2_PMAX;
        uint32_t fcmgt = is64 ? 0x6EF1E610u : 0x6EB1E610u;
        pop_q(c, V17); pop_q(c, V16); // b, a
        if (is_max) a64_neon(e, fcmgt, V2, V16, V17); // mask = a > b
        else        a64_neon(e, fcmgt, V2, V17, V16); // mask = b > a
        a64_neon(e, 0x6E721E30u, V2, V17, V16);       // bsl v2 = (m&b)|(~m&a)
        push_q(c, V2);
        return true;
    }
    case EA_OP_I32X4_DOT_I16X8_S:
        // smull/smull2 the even-lane products, addp the adjacent pairs:
        // lane[i] = a[2i]*b[2i] + a[2i+1]*b[2i+1]
        pop_q(c, V17); pop_q(c, V16);
        a64_neon(e, 0x0E71C210u, V2, V16, V17);        // smull v2.4s, a.4h, b.4h
        a64_neon(e, 0x4E71C210u, V3, V16, V17);        // smull2 v3.4s, a.8h, b.8h
        a64_neon(e, 0x4EB1BE10u, V16, V2, V3);         // addp v16.4s, v2.4s, v3.4s
        push_q(c, V16);
        return true;
    // ---- relaxed-simd (the implementation-defined behaviors land on the
    // matching NEON instructions; the suite asserts the interpreter's choice,
    // which these sequences reproduce) ----
    case EA_OP_I8X16_RELAXED_SWIZZLE:
        pop_q(c, V17); pop_q(c, V16);
        a64_neon(e, 0x4E110210u, V16, V16, V17); // tbl (OOB -> 0)
        push_q(c, V16);
        return true;
    case EA_OP_I8X16_RELAXED_LANESELECT: case EA_OP_I16X8_RELAXED_LANESELECT:
    case EA_OP_I32X4_RELAXED_LANESELECT: case EA_OP_I64X2_RELAXED_LANESELECT:
        pop_q(c, V18); pop_q(c, V17); pop_q(c, V16); // sel, x, y
        a64_neon(e, 0x6E721E30u, V18, V17, V16);     // bsl (m&x)|(~m&y)
        push_q(c, V18);
        return true;
    case EA_OP_F32X4_RELAXED_MIN: case EA_OP_F32X4_RELAXED_MAX:
    case EA_OP_F64X2_RELAXED_MIN: case EA_OP_F64X2_RELAXED_MAX: {
        // the relaxed spec allows IEEE minNum/maxNum directly
        bool is64 = in->opcode == EA_OP_F64X2_RELAXED_MIN || in->opcode == EA_OP_F64X2_RELAXED_MAX;
        pop_q(c, V17); pop_q(c, V16);
        a64_neon(e, is64 ? 0x4EF1F610u : 0x4EB1F610u, V16, V16, V17); // fmin
        a64_neon(e, is64 ? 0x4E71F610u : 0x4E31F610u, V16, V16, V17); // fmax
        push_q(c, V16);
        return true;
    }
    case EA_OP_F32X4_RELAXED_MADD: case EA_OP_F32X4_RELAXED_NMADD:
    case EA_OP_F64X2_RELAXED_MADD: case EA_OP_F64X2_RELAXED_NMADD: {
        bool is64 = in->opcode == EA_OP_F64X2_RELAXED_MADD || in->opcode == EA_OP_F64X2_RELAXED_NMADD;
        bool is_nm = in->opcode == EA_OP_F32X4_RELAXED_NMADD || in->opcode == EA_OP_F64X2_RELAXED_NMADD;
        pop_q(c, V18); pop_q(c, V17); pop_q(c, V16); // c, b, a
        if (is_nm) a64_neon(e, is64 ? 0x4EF1CE12u : 0x4EB1CE12u, V18, V16, V17); // fmls c -= a*b
        else       a64_neon(e, is64 ? 0x4E71CE12u : 0x4E31CE12u, V18, V16, V17); // fmla c += a*b
        push_q(c, V18);
        return true;
    }
    case EA_OP_I16X8_RELAXED_Q15MULR_S:
        pop_q(c, V17); pop_q(c, V16);
        a64_neon(e, 0x6E71B610u, V16, V16, V17); // sqrdmulh
        push_q(c, V16);
        return true;
    case EA_OP_I16X8_RELAXED_DOT_I8X16_I7X16_S:
        // pair products (smull/smull2), pairwise-add long (saddlp), then
        // saturating narrow into the two halves
        pop_q(c, V17); pop_q(c, V16);
        a64_neon(e, 0x0E71C210u, V2, V16, V17); // smull v2.8h (p0..p7)
        a64_neon(e, 0x4E71C210u, V3, V16, V17); // smull2 v3.8h (p8..p15)
        a64_neon(e, 0x4E602844u, V4, V2, 0);    // saddlp v4.4s (p0+p1..p6+p7)
        a64_neon(e, 0x4E602864u, V5, V3, 0);    // saddlp v5.4s (p8+p9..p14+p15)
        a64_neon(e, 0x0E614890u, V16, V4, 0);   // sqxtn v16.4h (lanes 0-3)
        a64_neon(e, 0x4E6148B0u, V16, V5, 17);  // sqxtn2 v16.8h (lanes 4-7)
        push_q(c, V16);
        return true;
    case EA_OP_I32X4_RELAXED_DOT_I8X16_I7X16_ADD_S:
        // sdot (the 4-term i8 dot per lane) + the accumulator
        pop_q(c, V18); pop_q(c, V17); pop_q(c, V16); // c, b, a
        a64_neon(e, 0x4E919610u, V2, V16, V17);      // sdot v2.4s, a, b
        a64_neon(e, 0x4EB18610u, V2, V2, V18);       // add the accumulator
        push_q(c, V2);
        return true;
    case EA_OP_I32X4_RELAXED_TRUNC_F32X4_S:
        pop_q(c, V16);
        a64_neon(e, 0x4EA1BA10u, V16, V16, 0); // fcvtzs (saturating)
        push_q(c, V16);
        return true;
    case EA_OP_I32X4_RELAXED_TRUNC_F32X4_U:
        pop_q(c, V16);
        a64_neon(e, 0x6EA1BA10u, V16, V16, 0); // fcvtzu (saturating)
        push_q(c, V16);
        return true;
    case EA_OP_I32X4_RELAXED_TRUNC_F64X2_S_ZERO: case EA_OP_I32X4_RELAXED_TRUNC_F64X2_U_ZERO: {
        bool u = in->opcode == EA_OP_I32X4_RELAXED_TRUNC_F64X2_U_ZERO;
        pop_q(c, V16);
        a64_neon(e, u ? 0x6EE1BA02u : 0x4EE1BA02u, V2, V16, 0); // fcvtzs/zu v2.2d (saturating)
        a64_neon(e, 0x6F00E410u, V16, 0, 0);            // movi v16.2d, #0
        a64_neon(e, 0x0EA12850u, V16, V2, 0);           // xtn v16.2s, v2.2d
        push_q(c, V16);
        return true;
    }
    }
    // ---- batch 3: comparisons, shifts, FP arith, int min/max ----
    // integer lane width: 16b/8h/4s/2d -> 0/1/2/3
    uint32_t w;
    switch (in->opcode) {
    case EA_OP_I8X16_EQ: case EA_OP_I8X16_NE: case EA_OP_I8X16_LT_S:
    case EA_OP_I8X16_LE_S: case EA_OP_I8X16_GT_S: case EA_OP_I8X16_GE_S:
    case EA_OP_I8X16_LT_U: case EA_OP_I8X16_LE_U: case EA_OP_I8X16_GT_U:
    case EA_OP_I8X16_GE_U: case EA_OP_I8X16_ADD: case EA_OP_I8X16_SUB:
    case EA_OP_I8X16_MIN_S: case EA_OP_I8X16_MIN_U: case EA_OP_I8X16_MAX_S:
    case EA_OP_I8X16_MAX_U:
    case EA_OP_I8X16_SHL: case EA_OP_I8X16_SHR_S: case EA_OP_I8X16_SHR_U:
        w = 0; break;
    case EA_OP_I16X8_EQ: case EA_OP_I16X8_NE: case EA_OP_I16X8_LT_S:
    case EA_OP_I16X8_LE_S: case EA_OP_I16X8_GT_S: case EA_OP_I16X8_GE_S:
    case EA_OP_I16X8_LT_U: case EA_OP_I16X8_LE_U: case EA_OP_I16X8_GT_U:
    case EA_OP_I16X8_GE_U: case EA_OP_I16X8_ADD: case EA_OP_I16X8_SUB:
    case EA_OP_I16X8_MUL: case EA_OP_I16X8_MIN_S: case EA_OP_I16X8_MIN_U:
    case EA_OP_I16X8_MAX_S: case EA_OP_I16X8_MAX_U:
    case EA_OP_I16X8_SHL: case EA_OP_I16X8_SHR_S: case EA_OP_I16X8_SHR_U:
        w = 1; break;
    case EA_OP_I32X4_EQ: case EA_OP_I32X4_NE: case EA_OP_I32X4_LT_S:
    case EA_OP_I32X4_LE_S: case EA_OP_I32X4_GT_S: case EA_OP_I32X4_GE_S:
    case EA_OP_I32X4_LT_U: case EA_OP_I32X4_LE_U: case EA_OP_I32X4_GT_U:
    case EA_OP_I32X4_GE_U: case EA_OP_I32X4_ADD: case EA_OP_I32X4_SUB:
    case EA_OP_I32X4_MUL: case EA_OP_I32X4_MIN_S: case EA_OP_I32X4_MIN_U:
    case EA_OP_I32X4_MAX_S: case EA_OP_I32X4_MAX_U:
    case EA_OP_I32X4_SHL: case EA_OP_I32X4_SHR_S: case EA_OP_I32X4_SHR_U:
        w = 2; break;
    case EA_OP_I64X2_EQ: case EA_OP_I64X2_NE: case EA_OP_I64X2_ADD:
    case EA_OP_I64X2_SUB:
        w = 3; break;
    default: return false;
    }
    // three-same samples calibrated at .16b (integer) / .4s (FP)
    static const uint32_t cmeq_b = 0x6E318E10u, cmgt_b = 0x4E313610u,
                          cmge_b = 0x4E313E10u, cmhi_b = 0x6E313610u,
                          cmhs_b = 0x6E313E10u, smin_b = 0x4E316E10u,
                          umin_b = 0x6E316E10u, smax_b = 0x4E316610u,
                          umax_b = 0x6E316610u, sshl_b = 0x4E314610u,
                          ushl_b = 0x6E314610u, mvn_s   = 0x6E205A10u;
    static const uint32_t fcmeq_s = 0x4E31E610u, fcmge_s = 0x6E31E610u,
                          fcmgt_s = 0x6EB1E610u, fadd_s  = 0x4E31D610u,
                          fsub_s  = 0x4EB1D610u, fmul_s  = 0x6E31DE10u,
                          fdiv_s  = 0x6E31FE10u;
    switch (in->opcode) {
    case EA_OP_I8X16_EQ: case EA_OP_I16X8_EQ: case EA_OP_I32X4_EQ:
    case EA_OP_I64X2_EQ:
        pop_q(c, V17); pop_q(c, V16);
        a64_neon_w(e, cmeq_b, w, V16, V16, V17);
        push_q(c, V16);
        return true;
    case EA_OP_I8X16_NE: case EA_OP_I16X8_NE: case EA_OP_I32X4_NE:
    case EA_OP_I64X2_NE:
        pop_q(c, V17); pop_q(c, V16);
        a64_neon_w(e, cmeq_b, w, V16, V16, V17);
        a64_neon(e, mvn_s, V16, V16, 0);
        push_q(c, V16);
        return true;
    case EA_OP_I8X16_GT_S: case EA_OP_I16X8_GT_S: case EA_OP_I32X4_GT_S:
    case EA_OP_I8X16_LT_S: case EA_OP_I16X8_LT_S: case EA_OP_I32X4_LT_S:
    case EA_OP_I8X16_GE_S: case EA_OP_I16X8_GE_S: case EA_OP_I32X4_GE_S:
    case EA_OP_I8X16_LE_S: case EA_OP_I16X8_LE_S: case EA_OP_I32X4_LE_S:
    case EA_OP_I8X16_GT_U: case EA_OP_I16X8_GT_U: case EA_OP_I32X4_GT_U:
    case EA_OP_I8X16_LT_U: case EA_OP_I16X8_LT_U: case EA_OP_I32X4_LT_U:
    case EA_OP_I8X16_GE_U: case EA_OP_I16X8_GE_U: case EA_OP_I32X4_GE_U:
    case EA_OP_I8X16_LE_U: case EA_OP_I16X8_LE_U: case EA_OP_I32X4_LE_U: {
        // a OP b via the direct or operand-swapped compare:
        //   gt_s: cmgt(a,b)  lt_s: cmgt(b,a)  ge_s: cmge(a,b)  le_s: cmge(b,a)
        //   gt_u: cmhi(a,b)  lt_u: cmhi(b,a)  ge_u: cmhs(a,b)  le_u: cmhs(b,a)
        uint32_t sample; bool swap;
        switch (in->opcode) {
        case EA_OP_I8X16_GT_S: case EA_OP_I16X8_GT_S: case EA_OP_I32X4_GT_S:
            sample = cmgt_b; swap = false; break;
        case EA_OP_I8X16_LT_S: case EA_OP_I16X8_LT_S: case EA_OP_I32X4_LT_S:
            sample = cmgt_b; swap = true; break;
        case EA_OP_I8X16_GE_S: case EA_OP_I16X8_GE_S: case EA_OP_I32X4_GE_S:
            sample = cmge_b; swap = false; break;
        case EA_OP_I8X16_LE_S: case EA_OP_I16X8_LE_S: case EA_OP_I32X4_LE_S:
            sample = cmge_b; swap = true; break;
        case EA_OP_I8X16_GT_U: case EA_OP_I16X8_GT_U: case EA_OP_I32X4_GT_U:
            sample = cmhi_b; swap = false; break;
        case EA_OP_I8X16_LT_U: case EA_OP_I16X8_LT_U: case EA_OP_I32X4_LT_U:
            sample = cmhi_b; swap = true; break;
        case EA_OP_I8X16_GE_U: case EA_OP_I16X8_GE_U: case EA_OP_I32X4_GE_U:
            sample = cmhs_b; swap = false; break;
        default: sample = cmhs_b; swap = true; break;
        }
        pop_q(c, V17); pop_q(c, V16);
        if (swap) a64_neon_w(e, sample, w, V16, V17, V16);
        else      a64_neon_w(e, sample, w, V16, V16, V17);
        push_q(c, V16);
        return true;
    }
    case EA_OP_I8X16_MIN_S: case EA_OP_I16X8_MIN_S: case EA_OP_I32X4_MIN_S:
    case EA_OP_I8X16_MIN_U: case EA_OP_I16X8_MIN_U: case EA_OP_I32X4_MIN_U:
    case EA_OP_I8X16_MAX_S: case EA_OP_I16X8_MAX_S: case EA_OP_I32X4_MAX_S:
    case EA_OP_I8X16_MAX_U: case EA_OP_I16X8_MAX_U: case EA_OP_I32X4_MAX_U: {
        pop_q(c, V17); pop_q(c, V16);
        uint32_t sample;
        switch (in->opcode) {
        case EA_OP_I8X16_MIN_S: case EA_OP_I16X8_MIN_S: case EA_OP_I32X4_MIN_S: sample = smin_b; break;
        case EA_OP_I8X16_MIN_U: case EA_OP_I16X8_MIN_U: case EA_OP_I32X4_MIN_U: sample = umin_b; break;
        case EA_OP_I8X16_MAX_S: case EA_OP_I16X8_MAX_S: case EA_OP_I32X4_MAX_S: sample = smax_b; break;
        default: sample = umax_b; break;
        }
        a64_neon_w(e, sample, w, V16, V16, V17);
        push_q(c, V16);
        return true;
    }
    case EA_OP_I8X16_SHL: case EA_OP_I16X8_SHL: case EA_OP_I32X4_SHL:
    case EA_OP_I8X16_SHR_S: case EA_OP_I16X8_SHR_S: case EA_OP_I32X4_SHR_S:
    case EA_OP_I8X16_SHR_U: case EA_OP_I16X8_SHR_U: case EA_OP_I32X4_SHR_U:
    case EA_OP_I64X2_SHL: case EA_OP_I64X2_SHR_S: case EA_OP_I64X2_SHR_U: {
        // variable-count vector shift via sshl/ushl: the count is masked to
        // the lane width (right shifts encode a negative count)
        bool is_shl = in->opcode == EA_OP_I8X16_SHL || in->opcode == EA_OP_I16X8_SHL ||
                      in->opcode == EA_OP_I32X4_SHL || in->opcode == EA_OP_I64X2_SHL;
        bool is_shr_s = in->opcode == EA_OP_I8X16_SHR_S || in->opcode == EA_OP_I16X8_SHR_S ||
                        in->opcode == EA_OP_I32X4_SHR_S || in->opcode == EA_OP_I64X2_SHR_S;
        uint32_t es = w == 0 ? 8 : w == 1 ? 16 : w == 2 ? 32 : 64;
        pop_w(e, R16); // shift count (i32)
        pop_q(c, V16);
        // count mod esize (a64_and_imm32 takes a PRE-ENCODED bitmask field,
        // not a plain immediate — materialize the mask in a register instead)
        a64_mov32_imm(e, R17, es - 1);
        a64_and_reg32(e, R16, R16, R17);
        // sshl/ushl register form: negative count = right shift by |count|
        // (NOT the esize-shift immediate convention)
        if (!is_shl) a64_neg32(e, R16, R16);
        a64_neon_dup(e, 1u << w, V17, R16); // broadcast at the lane width
        a64_neon_w(e, is_shr_s ? sshl_b : ushl_b, w, V16, V16, V17);
        push_q(c, V16);
        return true;
    }
    case EA_OP_F32X4_ADD: case EA_OP_F32X4_SUB: case EA_OP_F32X4_MUL:
    case EA_OP_F32X4_DIV: case EA_OP_F64X2_ADD: case EA_OP_F64X2_SUB:
    case EA_OP_F64X2_MUL: case EA_OP_F64X2_DIV: {
        uint32_t sample;
        switch (in->opcode) {
        case EA_OP_F32X4_ADD: sample = fadd_s; break;
        case EA_OP_F32X4_SUB: sample = fsub_s; break;
        case EA_OP_F32X4_MUL: sample = fmul_s; break;
        case EA_OP_F32X4_DIV: sample = fdiv_s; break;
        case EA_OP_F64X2_ADD: sample = fadd_s; break;
        case EA_OP_F64X2_SUB: sample = fsub_s; break;
        case EA_OP_F64X2_MUL: sample = fmul_s; break;
        default: sample = fdiv_s; break;
        }
        uint32_t fw = (in->opcode >= EA_OP_F64X2_ADD) ? 1 : 0;
        pop_q(c, V17); pop_q(c, V16);
        a64_neon_w(e, sample, fw, V16, V16, V17);
        push_q(c, V16);
        return true;
    }
    case EA_OP_F32X4_EQ: case EA_OP_F32X4_NE: case EA_OP_F32X4_LT:
    case EA_OP_F32X4_LE: case EA_OP_F32X4_GT: case EA_OP_F32X4_GE:
    case EA_OP_F64X2_EQ: case EA_OP_F64X2_NE: case EA_OP_F64X2_LT:
    case EA_OP_F64X2_LE: case EA_OP_F64X2_GT: case EA_OP_F64X2_GE: {
        uint32_t sample; bool swap;
        switch (in->opcode) {
        case EA_OP_F32X4_EQ: sample = fcmeq_s; swap = false; break;
        case EA_OP_F32X4_NE: sample = fcmeq_s; swap = false; break;
        case EA_OP_F32X4_GT: sample = fcmgt_s; swap = false; break;
        case EA_OP_F32X4_LT: sample = fcmgt_s; swap = true; break;
        case EA_OP_F32X4_GE: sample = fcmge_s; swap = false; break;
        case EA_OP_F32X4_LE: sample = fcmge_s; swap = true; break;
        case EA_OP_F64X2_EQ: sample = fcmeq_s; swap = false; break;
        case EA_OP_F64X2_NE: sample = fcmeq_s; swap = false; break;
        case EA_OP_F64X2_GT: sample = fcmgt_s; swap = false; break;
        case EA_OP_F64X2_LT: sample = fcmgt_s; swap = true; break;
        case EA_OP_F64X2_GE: sample = fcmge_s; swap = false; break;
        default: sample = fcmge_s; swap = true; break;
        }
        uint32_t fw = (in->opcode >= EA_OP_F64X2_EQ) ? 1 : 0;
        pop_q(c, V17); pop_q(c, V16);
        if (swap) a64_neon_w(e, sample, fw, V16, V17, V16);
        else      a64_neon_w(e, sample, fw, V16, V16, V17);
        if (in->opcode == EA_OP_F32X4_NE || in->opcode == EA_OP_F64X2_NE)
            a64_neon(e, mvn_s, V16, V16, 0);
        push_q(c, V16);
        return true;
    }
    }
    return false;
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
    case EA_OP_I32_EQ: pop_pair_w(c); a64_cmp_reg32(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_EQ); return true;
    case EA_OP_I32_NE: pop_pair_w(c); a64_cmp_reg32(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_NE); return true;
    case EA_OP_I32_LT_S: pop_pair_w(c); a64_cmp_reg32(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_LT); return true;
    case EA_OP_I32_LT_U: pop_pair_w(c); a64_cmp_reg32(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_LO); return true;
    case EA_OP_I32_GT_S: pop_pair_w(c); a64_cmp_reg32(e, c->emit_b, c->emit_a); cmp_result_w(c, CC_LT); return true;
    case EA_OP_I32_GT_U: pop_pair_w(c); a64_cmp_reg32(e, c->emit_b, c->emit_a); cmp_result_w(c, CC_LO); return true;
    case EA_OP_I32_LE_S: pop_pair_w(c); a64_cmp_reg32(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_LE); return true;
    case EA_OP_I32_LE_U: pop_pair_w(c); a64_cmp_reg32(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_LS); return true;
    case EA_OP_I32_GE_S: pop_pair_w(c); a64_cmp_reg32(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_GE); return true;
    case EA_OP_I32_GE_U: pop_pair_w(c); a64_cmp_reg32(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_HS); return true;
    // ---- i64 comparisons (result i32)
    case EA_OP_I64_EQZ:
        pop_x(e, R16); a64_cmp_reg64(e, R16, 31); cmp_result_w(c, CC_EQ); return true;
    case EA_OP_I64_EQ: pop_pair_x(c); a64_cmp_reg64(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_EQ); return true;
    case EA_OP_I64_NE: pop_pair_x(c); a64_cmp_reg64(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_NE); return true;
    case EA_OP_I64_LT_S: pop_pair_x(c); a64_cmp_reg64(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_LT); return true;
    case EA_OP_I64_LT_U: pop_pair_x(c); a64_cmp_reg64(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_LO); return true;
    case EA_OP_I64_GT_S: pop_pair_x(c); a64_cmp_reg64(e, c->emit_b, c->emit_a); cmp_result_w(c, CC_LT); return true;
    case EA_OP_I64_GT_U: pop_pair_x(c); a64_cmp_reg64(e, c->emit_b, c->emit_a); cmp_result_w(c, CC_LO); return true;
    case EA_OP_I64_LE_S: pop_pair_x(c); a64_cmp_reg64(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_LE); return true;
    case EA_OP_I64_LE_U: pop_pair_x(c); a64_cmp_reg64(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_LS); return true;
    case EA_OP_I64_GE_S: pop_pair_x(c); a64_cmp_reg64(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_GE); return true;
    case EA_OP_I64_GE_U: pop_pair_x(c); a64_cmp_reg64(e, c->emit_a, c->emit_b); cmp_result_w(c, CC_HS); return true;
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
    case EA_OP_I32_ADD: { uint32_t d = binop_dst(c, in); pop_pair_w(c);
        if (c->pop_imm_ok) { uint32_t iv = c->pop_imm;
            if (iv <= 4095) a64_add_imm32(e, d, c->emit_a, iv);
            else a64_sub_imm32(e, d, c->emit_a, 0 - iv);  // the negated fold
        } else a64_add_reg32(e, d, c->emit_a, c->emit_b);
        push_result_w(c); return true; }
    case EA_OP_I32_SUB: { uint32_t d = binop_dst(c, in); pop_pair_w(c);
        if (c->pop_imm_ok) { uint32_t iv = c->pop_imm;
            if (iv <= 4095) a64_sub_imm32(e, d, c->emit_a, iv);
            else a64_add_imm32(e, d, c->emit_a, 0 - iv);  // the negated fold
        } else a64_sub_reg32(e, d, c->emit_a, c->emit_b);
        push_result_w(c); return true; }
    case EA_OP_I32_MUL: { uint32_t d = binop_dst(c, in); pop_pair_w(c); a64_madd32(e, d, c->emit_a, c->emit_b, 31); push_result_w(c); return true; }
    case EA_OP_I32_AND: { uint32_t d = binop_dst(c, in); pop_pair_w(c); a64_and_reg32(e, d, c->emit_a, c->emit_b); push_result_w(c); return true; }
    case EA_OP_I32_OR: { uint32_t d = binop_dst(c, in); pop_pair_w(c); a64_orr_reg32(e, d, c->emit_a, c->emit_b); push_result_w(c); return true; }
    case EA_OP_I32_XOR: { uint32_t d = binop_dst(c, in); pop_pair_w(c); a64_eor_reg32(e, d, c->emit_a, c->emit_b); push_result_w(c); return true; }
    case EA_OP_I32_SHL: { uint32_t d = binop_dst(c, in); pop_pair_w(c); a64_lslv32(e, d, c->emit_a, c->emit_b); push_result_w(c); return true; }
    case EA_OP_I32_SHR_S: { uint32_t d = binop_dst(c, in); pop_pair_w(c); a64_asrv32(e, d, c->emit_a, c->emit_b); push_result_w(c); return true; }
    case EA_OP_I32_SHR_U: { uint32_t d = binop_dst(c, in); pop_pair_w(c); a64_lsrv32(e, d, c->emit_a, c->emit_b); push_result_w(c); return true; }
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
    case EA_OP_I64_ADD: { uint32_t d = binop_dst(c, in); pop_pair_x(c);
        if (c->pop_imm_ok) { uint32_t iv = c->pop_imm;
            if (iv <= 4095) a64_add_imm64(e, d, c->emit_a, iv);
            else a64_sub_imm64(e, d, c->emit_a, 0 - iv);  // the negated fold
        } else a64_add_reg64(e, d, c->emit_a, c->emit_b);
        push_result_x(c); return true; }
    case EA_OP_I64_SUB: { uint32_t d = binop_dst(c, in); pop_pair_x(c);
        if (c->pop_imm_ok) { uint32_t iv = c->pop_imm;
            if (iv <= 4095) a64_sub_imm64(e, d, c->emit_a, iv);
            else a64_add_imm64(e, d, c->emit_a, 0 - iv);  // the negated fold
        } else a64_sub_reg64(e, d, c->emit_a, c->emit_b);
        push_result_x(c); return true; }
    case EA_OP_I64_MUL: { uint32_t d = binop_dst(c, in); pop_pair_x(c); a64_madd64(e, d, c->emit_a, c->emit_b, 31); push_result_x(c); return true; }
    case EA_OP_I64_AND: { uint32_t d = binop_dst(c, in); pop_pair_x(c); a64_and_reg64(e, d, c->emit_a, c->emit_b); push_result_x(c); return true; }
    case EA_OP_I64_OR: { uint32_t d = binop_dst(c, in); pop_pair_x(c); a64_orr_reg64(e, d, c->emit_a, c->emit_b); push_result_x(c); return true; }
    case EA_OP_I64_XOR: { uint32_t d = binop_dst(c, in); pop_pair_x(c); a64_eor_reg64(e, d, c->emit_a, c->emit_b); push_result_x(c); return true; }
    case EA_OP_I64_SHL: { uint32_t d = binop_dst(c, in); pop_pair_x(c); a64_lslv64(e, d, c->emit_a, c->emit_b); push_result_x(c); return true; }
    case EA_OP_I64_SHR_S: { uint32_t d = binop_dst(c, in); pop_pair_x(c); a64_asrv64(e, d, c->emit_a, c->emit_b); push_result_x(c); return true; }
    case EA_OP_I64_SHR_U: { uint32_t d = binop_dst(c, in); pop_pair_x(c); a64_lsrv64(e, d, c->emit_a, c->emit_b); push_result_x(c); return true; }
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
            uint32_t nx2 = c->cur_pc + 1;
            uint32_t nop2 = nx2 < (uint32_t)c->f->code.n ? c->f->code.v[nx2].opcode : 0;
            uint32_t iv = in->imm.u32;
            if ((nop2 == EA_OP_I32_ADD || nop2 == EA_OP_I32_SUB) &&
                (iv <= 4095 || iv >= UINT32_MAX - 4094)) {
                c->def_b_imm = true; c->def_imm = iv;
                c->def_second_reg = 0;
            } else {
                a64_mov32_imm(e, R17, in->imm.u32);
                c->def_b_imm = false;
                c->def_second_reg = 0;
            }
            c->def_kind1 = 0; c->def_count = 2; c->def_first = 0;
        } else if (c->def_count == 0 && c->def2_live && defer2_possible(c, c->cur_pc, c->f->code.n)) {
            a64_mov32_imm(e, R16, in->imm.u32); // middle of a def2 triple
            c->def_kind0 = 0; c->def_count = 1; c->def_first = 1;
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
            uint32_t nx2 = c->cur_pc + 1;
            uint32_t nop2 = nx2 < (uint32_t)c->f->code.n ? c->f->code.v[nx2].opcode : 0;
            uint64_t iv = in->imm.u64;
            if ((nop2 == EA_OP_I64_ADD || nop2 == EA_OP_I64_SUB) &&
                (iv <= 4095 || iv >= UINT64_MAX - 4094)) {
                // the consumer folds the constant into its imm12 form: no
                // register materialization at all
                c->def_b_imm = true; c->def_imm = (uint32_t)iv;
                c->def_second_reg = 0;
            } else {
                a64_mov64_imm(e, R17, in->imm.u64);
                c->def_b_imm = false;
                c->def_second_reg = 0;
            }
            c->def_kind1 = 1; c->def_count = 2; c->def_first = 0;
        } else if (c->def_count == 0 && c->def2_live && defer2_possible(c, c->cur_pc, c->f->code.n)) {
            a64_mov64_imm(e, R16, in->imm.u64); // middle of a def2 triple
            c->def_kind0 = 1; c->def_count = 1; c->def_first = 1;
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
    case EA_OP_I32_TRUNC_SAT_F32_S: { uint32_t v = trunc_sat_src(c, 4); a64_fcvtzs(e, R16, v, 4, 4); push_w(e, R16); return true; }
    case EA_OP_I32_TRUNC_SAT_F32_U: { uint32_t v = trunc_sat_src(c, 4); a64_fcvtzu(e, R16, v, 4, 4); push_w(e, R16); return true; }
    case EA_OP_I32_TRUNC_SAT_F64_S: { uint32_t v = trunc_sat_src(c, 8); a64_fcvtzs(e, R16, v, 8, 4); push_w(e, R16); return true; }
    case EA_OP_I32_TRUNC_SAT_F64_U: { uint32_t v = trunc_sat_src(c, 8); a64_fcvtzu(e, R16, v, 8, 4); push_w(e, R16); return true; }
    case EA_OP_I64_TRUNC_SAT_F32_S: { uint32_t v = trunc_sat_src(c, 4); a64_fcvtzs(e, R16, v, 4, 8); push_x(e, R16); return true; }
    case EA_OP_I64_TRUNC_SAT_F32_U: { uint32_t v = trunc_sat_src(c, 4); a64_fcvtzu(e, R16, v, 4, 8); push_x(e, R16); return true; }
    case EA_OP_I64_TRUNC_SAT_F64_S: { uint32_t v = trunc_sat_src(c, 8); a64_fcvtzs(e, R16, v, 8, 8); push_x(e, R16); return true; }
    case EA_OP_I64_TRUNC_SAT_F64_U: { uint32_t v = trunc_sat_src(c, 8); a64_fcvtzu(e, R16, v, 8, 8); push_x(e, R16); return true; }
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
        if (c->n_cref) { c->n_cref--; fmov_to_fpr(c, V0, cref_reg(c, c->n_cref), 4); }
        else pop_s(&c->em, V0);
    } else {
        flush_deferred(c); // parked int operands must reach the stack first
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
        if (c->n_cref) { c->n_cref--; fmov_to_fpr(c, V0, cref_reg(c, c->n_cref), 8); }
        else pop_d(&c->em, V0);
    } else {
        flush_deferred(c); // parked int operands must reach the stack first
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
        trap_if(c, TRAP_INT_OVERFLOW, CC_EQ);
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
        trap_if(c, TRAP_INT_OVERFLOW, CC_EQ);
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
    // resolve exception clause targets to machine word offsets (absolutized
    // at publish time, once the code block has its final address)
    for (uint32_t i = 0; i < c->n_eh_fx; i++) {
        struct EhFx *fx = &c->eh_fx[i];
        uint32_t eoff = fx->func_level ? c->eh_ret_at : c->insn_at[fx->tpc];
        if (eoff == UINT32_MAX) return false;
        fx->desc->c[fx->clause].target_off = eoff;
    }
    return true;
}

// once any module uses exception handling, later-compiled modules get checked
// call sites too: their imports may resolve to throwing functions compiled
// earlier (import linking forces the source module to be compiled first)
static bool g_any_eh_jit = false;

void ea_jit_compile_module(EaModule *m) {
    // call sites need the marker check when exceptions are in play anywhere
    bool any_indirect = false;
    for (uint32_t fi = 0; fi < m->n_funcs && !any_indirect; fi++) {
        EaFunc *f = &m->funcs[fi];
        if (f->code.n == 0) continue;
        for (uint32_t q = 0; q < f->code.n; q++) {
            uint32_t op = f->code.v[q].opcode;
            if (op == EA_OP_CALL_INDIRECT || op == EA_OP_CALL_REF) { any_indirect = true; break; }
        }
    }
    bool eh_calls = m->feat.exceptions || g_any_eh_jit || any_indirect;
    // size the exception clause descriptor arena and place it in the code
    // region (final addresses are needed at handler-push emission time)
    // the whole compile writes into the region (descriptors, code blocks):
    // make it writable, restore exec-only when done (execution toggles are
    // per-thread and ea_jit_call leaves the region write-protected)
    pthread_jit_write_protect_np(0);
    g_eh_cursor = g_eh_end = NULL;
    size_t eh_bytes = 0;
    if (m->feat.exceptions) {
        g_any_eh_jit = true;
        for (uint32_t fi = 0; fi < m->n_funcs; fi++) {
            EaFunc *f = &m->funcs[fi];
            if (f->code.n == 0) continue;
            for (uint32_t q = 0; q < f->code.n; q++)
                if (f->code.v[q].opcode == EA_OP_TRY_TABLE) {
                    size_t sz = sizeof(EaEhDesc) +
                        (f->code.v[q].n_catches ? f->code.v[q].n_catches : 1) * sizeof(EaEhClause);
                    eh_bytes += (sz + 15) & ~(size_t)15;
                }
        }
    }
    if (eh_bytes) {
        uint8_t *arena = region_alloc(eh_bytes);
        if (arena) {
            g_eh_cursor = arena;
            g_eh_end = arena + eh_bytes;
        }
    }
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
        c.eh_calls = eh_calls;
        c.is_leaf = 1;
        for (uint32_t q = 0; q < f->code.n; q++) {
            uint32_t op = f->code.v[q].opcode;
            if (op == EA_OP_THROW) c.has_throw = 1;
            else if (op == EA_OP_THROW_REF) c.has_throwref = 1;
            switch (op) {
            case EA_OP_CALL: case EA_OP_CALL_INDIRECT: case EA_OP_CALL_REF:
            case EA_OP_RETURN_CALL: case EA_OP_RETURN_CALL_INDIRECT:
            case EA_OP_RETURN_CALL_REF:
            case EA_OP_THROW: case EA_OP_THROW_REF: case EA_OP_TRY_TABLE:
                c.is_leaf = 0; // frame may grow without this function's check
                break;
            }
        }
        // v128 params/locals: the 1:1 model moves 8-byte scalars through the
        // frame (param copy, local get/set) — leave such functions to the
        // interpreter, which handles full 16-byte slots
        {
            bool has_v128 = false;
            for (uint32_t q = 0; q < f->n_locals && q < 4096; q++)
                has_v128 |= f->locals[q] == VT_V128;
            if (has_v128)
                for (uint32_t q = 0; q < f->code.n; q++)
                    if (f->code.v[q].opcode == EA_OP_SELECT ||
                        f->code.v[q].opcode == EA_OP_SELECT_T) {
                        // the 8-byte csel select cannot carry a v128 payload
                        has_v128 = 2;
                        break;
                    }
            if (has_v128) { em_free(&c.em); free(c.eh_fx); free(c.eh_descs); continue; }
        }
        uint64_t fsz = 48 + (uint64_t)c.n_locals * 16 + ((uint64_t)f->max_stack + 8) * 16;
        if (fsz >= (1 << 20)) {
            em_free(&c.em);
            free(c.eh_fx);
            free(c.eh_descs);
            continue;
        }
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
            free(c.eh_fx);
            free(c.eh_descs);
            continue;
        }
        if (getenv("EA_JIT_STATS"))
            fprintf(stderr, "JIT ok func %u -> %u insns\n", fi, (uint32_t)c.em.len);
        uint8_t *dst = region_alloc(c.em.len * 4);
        if (!dst) {
            em_free(&c.em);
            free(c.fx); free(c.tfx); free(c.insn_at); free(c.is_target);
            free(c.eh_fx); free(c.eh_descs);
            continue;
        }
            pthread_jit_write_protect_np(0);
        memcpy(dst, c.em.buf, c.em.len * 4);
        // absolutize exception clause targets now that the code is placed
        for (uint32_t i = 0; i < c.n_eh_fx; i++) {
            struct EhFx *fx = &c.eh_fx[i];
            fx->desc->c[fx->clause].target = dst + (size_t)fx->desc->c[fx->clause].target_off * 4;
            if (getenv("EA_LDBG"))
                fprintf(stderr, "[LDBG] publish f%u dst=%p off=%u target=%p\n",
                        c.fn_idx, (void *)dst, fx->desc->c[fx->clause].target_off,
                        (void *)fx->desc->c[fx->clause].target);
        }

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
        free(c.eh_fx);
        free(c.eh_descs);
    }
    pthread_jit_write_protect_np(1); // exec-only again for execution
}
