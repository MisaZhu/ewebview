// easm interpreter: switch-based execution over pre-decoded instruction lists
#include "easm.h"
#include "opcodes.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>

int interp_ensure(EaExec *ex, uint32_t extra) {
    uint64_t need = (uint64_t)ex->sp + extra;
    if (need <= ex->stack_cap) return 0;
    uint64_t cap = ex->stack_cap ? ex->stack_cap : 4096;
    while (cap < need) cap *= 2;
    ex->stack = (WVal *)ea_realloc(ex->stack, cap * sizeof(WVal));
    ex->stack_cap = (uint32_t)cap;
    return 0;
}

#define TRAP(code) do { ea_trap(ex, (code)); return 1; } while (0)

static uint64_t load_width_bytes(uint32_t op) {
    switch (op) {
    case EA_OP_I32_LOAD8_S: case EA_OP_I32_LOAD8_U:
    case EA_OP_I64_LOAD8_S: case EA_OP_I64_LOAD8_U: return 1;
    case EA_OP_I32_LOAD16_S: case EA_OP_I32_LOAD16_U:
    case EA_OP_I64_LOAD16_S: case EA_OP_I64_LOAD16_U: return 2;
    case EA_OP_I32_LOAD: case EA_OP_F32_LOAD:
    case EA_OP_I64_LOAD32_S: case EA_OP_I64_LOAD32_U: return 4;
    default: return 8;
    }
}
static uint64_t store_width_bytes(uint32_t op) {
    switch (op) {
    case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: return 1;
    case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: return 2;
    case EA_OP_I32_STORE: case EA_OP_F32_STORE:
    case EA_OP_I64_STORE32: return 4;
    default: return 8;
    }
}
int exec_numeric(EaExec *ex, uint32_t op, WVal *S, uint32_t *spp);
int exec_simd(EaExec *ex, EaInstance *inst, const EaInstr *in, WVal *S, uint32_t *spp);

// ---------------------------------------------------------------- runtime ctrl frame
typedef struct {
    uint32_t height;
    uint32_t arity;     // br label arity (loop: params)
    uint32_t rarity;    // result arity on natural fallthrough
    uint32_t pc_end;
    uint32_t pc_else;
    uint32_t block_idx;
    uint8_t is_loop;
    uint8_t is_try;
    struct EaInstr *try_in; // try_table instruction (catch clauses)
} RCtl;

#define MAX_CTRL 4096

// execute function; args are the top n_params values on the stack.
// on success params replaced in place by results.
// ---------------- GC runtime ----------------
EaHeapObj *ea_gc_alloc(EaModule *m, uint32_t type_idx, uint32_t n) {
    EaHeapObj *o = (EaHeapObj *)ea_malloc(sizeof(EaHeapObj) + (n ? n : 1) * sizeof(WVal));
    o->magic = EA_HEAP_MAGIC;
    o->kind = m->types[type_idx].kind == CT_STRUCT ? EA_HK_STRUCT : EA_HK_ARRAY;
    o->type_idx = type_idx;
    o->length = n;
    memset(o->data, 0, (n ? n : 1) * sizeof(WVal));
    return o;
}
bool ea_gc_is_heap(void *p) {
    if (p == NULL || ea_is_i31(p) || (uintptr_t)p < 4096) return false;
    EaHeapObj *o = (EaHeapObj *)p;
    return o->magic == EA_HEAP_MAGIC && o->kind <= 4;
}
// structural canonical type equivalence (rec-group duplicates across the
// type space denote the same type at runtime) — shared implementation in
// validate.c (coinductive rec-group comparison)
static bool type_canon_eq(EaModule *m, uint32_t a, uint32_t b, int depth) {
    (void)depth;
    return ea_type_canon_eq1(m, a, b);
}

// does heap value `ref` match heap type `ht` (s33) with nullability `nullable`?
static bool ea_gc_match(EaModule *m, void *ref, int64_t ht, bool nullable) {
    if (ref == NULL) return nullable;
    if (ea_is_i31(ref)) {
        if (ht >= 0) return false;
        switch (ht) {
        case -0x14: case -0x13: case -0x12: return true; // i31 / eq / any
        default: return false;
        }
    }
    if (!ea_gc_is_heap(ref)) {
        // raw funcref (EaFuncInst*, large pointer) or extern/host value (small tag)
        if ((uintptr_t)ref < 4096)
            return ht == -0x11 || ht == -0x12; // extern / any (internalized host ref)
        if (ht >= 0) {
            // concrete func type: walk the funcref's declared type chain
            EaFuncInst *fi = (EaFuncInst *)ref;
            if (!fi->inst || !fi->inst->module) return false;
            EaModule *fm = fi->inst->module;
            for (uint32_t ti = fi->type_idx; ti < fm->n_types; ) {
                if (ti == (uint32_t)ht || type_canon_eq(fm, ti, (uint32_t)ht, 0)) return true;
                if (fm->types[ti].n_sup == 0) break;
                ti = fm->types[ti].sup[0];
            }
            return false;
        }
        return ht == -0x10;                    // func
    }
    EaHeapObj *o = (EaHeapObj *)ref;
    if (o->kind == 3) return ht == -0x12; // (legacy) any-side wrap
    if (o->kind == 4) return ht == -0x11; // extern-side wrap matches only extern
    if (ht >= 0) {
        uint32_t ti = o->type_idx;
        for (uint32_t round = 0; round < 64 && ti < m->n_types; round++) {
            if (type_canon_eq(m, ti, (uint32_t)ht, 0)) return true;
            if (m->types[ti].n_sup == 0) break;
            ti = m->types[ti].sup[0];
        }
        return false;
    }
    switch (ht) {
    case -0x12: return true;             // any
    case -0x13: return true;             // eq
    case -0x15: return o->kind == EA_HK_STRUCT;
    case -0x16: return o->kind == EA_HK_ARRAY;
    default: return false;
    }
}
// try to find a catch handler for `exn` in the current frame's control stack.
// returns NULL when handled (sp/csp/pc updated), else the exn to propagate.
static EaExnInst *eh_unwind(EaExec *ex, EaInstance *inst, RCtl *ctl, uint32_t *cspp,
                            WVal *S, uint32_t *spp, uint32_t *pcp, EaExnInst *exn) {
    (void)ex;
    uint32_t csp = *cspp, sp = *spp, pc = *pcp;
    (void)sp; (void)pc;
    for (uint32_t k = csp; k-- > 0; ) {
        if (!ctl[k].is_try) continue;
        EaInstr *tin = ctl[k].try_in;
        for (uint32_t ci = 0; ci < tin->n_catches; ci++) {
            EaCatch *cc = &tin->catches[ci];
            uint32_t ncarry = 0;
            bool match = false;
            if (cc->kind <= 1) {
                EaTagInst *want = &inst->tags[cc->tag];
                if (exn->tag->ident == want->ident) {
                    match = true;
                    ncarry = exn->n_vals + (cc->kind == 1 ? 1 : 0);
                }
            } else if (cc->kind == 2) {
                match = true;
                ncarry = 0;
            } else {
                match = true;
                ncarry = 1;
            }
            if (!match) continue;
            if (getenv("EA_GDBG")) {
                fprintf(stderr, "EH match k=%u kind=%u tag=%u l=%u nvals=%u sp=%u\n", k, cc->kind, cc->tag, cc->label, exn->n_vals, sp);
                for (uint32_t q = 0; q < csp; q++)
                    fprintf(stderr, "   ctl[%u] h=%u pc_end=%u try=%d\n", q, ctl[q].height, ctl[q].pc_end, (int)ctl[q].is_try);
            }
            // catch labels resolve OUTSIDE the try_table frame
            uint32_t l = cc->label;
            RCtl *t2 = &ctl[k - 1 - l];
            uint32_t h = t2->height;
            for (uint32_t q = 0; q < exn->n_vals; q++) S[h + q] = exn->vals[q];
            if (cc->kind == 1 || cc->kind == 3) S[h + exn->n_vals].ref = exn;
            *spp = h + ncarry;
            *pcp = t2->is_loop ? t2->block_idx + 1 : t2->pc_end;
            *cspp = k - l;
            return NULL;
        }
    }
    return exn;
}

int ea_interp_exec_function(EaExec *ex, EaFuncInst *fi) {
    if (ex->depth >= ex->max_depth) TRAP(TRAP_STACK_EXHAUSTED);
    EaInstance *inst = fi->inst;
    EaModule *m = inst->module;
    EaFunc *f = (EaFunc *)fi->code;
    EaFuncType *ft = fi->type;
    uint32_t n_params = ft->n_params;
    uint32_t n_results = ft->n_results;

    uint32_t base = ex->sp - n_params; // locals live at [base, base+n_locals)
    uint32_t n_locals = f->n_locals;
    if (interp_ensure(ex, n_locals + f->max_stack + 8) != 0)
        TRAP(TRAP_STACK_EXHAUSTED);
    // shift params down? params are already at [base..base+n_params) — locals region overlaps
    // zero-init non-param locals
    for (uint32_t i = n_params; i < n_locals; i++)
        memset(&ex->stack[base + i], 0, sizeof(WVal));
    ex->sp = base + n_locals;

    WVal *S = ex->stack;
    uint32_t sp = ex->sp;
    uint32_t hoff = base + n_locals; // validator heights are relative to operand base
    uint32_t ctl_cap = 32;
    RCtl *ctl = (RCtl *)ea_malloc(ctl_cap * sizeof(RCtl));
    uint32_t csp = 0;
    uint32_t pc = 0;
    int result = 0;

#define STACK_GUARD(n) do { if (interp_ensure(ex, (uint32_t)(n)) != 0) TRAP(TRAP_STACK_EXHAUSTED); S = ex->stack; } while (0)

    EaInstr *code = f->code.v;
    uint32_t n_instr = f->code.n;
#define EA_TRACE() do { if (getenv("EA_IDBG3") && ex->depth == 0) fprintf(stderr, "pc=%u op=%02x sp=%u base=%u csp=%u\n", pc, code[pc].opcode, sp, base, csp); } while (0)
    // synthetic function frame: br to the function label behaves like return
    ctl[0].height = base;
    ctl[0].arity = n_results;
    ctl[0].rarity = n_results;
    ctl[0].pc_end = n_instr;
    ctl[0].pc_else = UINT32_MAX;
    ctl[0].block_idx = UINT32_MAX;
    ctl[0].is_loop = 0;
    csp = 1;
#define CTL_PUSH() do { \
    if (csp == ctl_cap) { ctl_cap *= 2; ctl = (RCtl *)ea_realloc(ctl, ctl_cap * sizeof(RCtl)); } \
} while (0)
    while (pc < n_instr) {
        EA_TRACE();
        EaInstr *in = &code[pc];
        uint32_t op = in->opcode;
        switch (op) {
        case EA_OP_UNREACHABLE: TRAP(TRAP_UNREACHABLE);
        case EA_OP_NOP: pc++; break;
        case EA_OP_BLOCK: case EA_OP_LOOP: {
            CTL_PUSH();
            ctl[csp].height = in->height + hoff;
            ctl[csp].arity = in->arity_out;
            ctl[csp].rarity = in->arity_res;
            ctl[csp].pc_end = in->end_idx;
            ctl[csp].pc_else = UINT32_MAX;
            ctl[csp].block_idx = pc;
            ctl[csp].is_loop = op == EA_OP_LOOP;
            ctl[csp].is_try = 0;
            ctl[csp].try_in = NULL;
            csp++;
            pc++;
            break;
        }
        case EA_OP_TRY_TABLE: {
            CTL_PUSH();
            ctl[csp].height = in->height + hoff;
            ctl[csp].arity = in->arity_res;
            ctl[csp].rarity = in->arity_res;
            ctl[csp].pc_end = in->end_idx;
            ctl[csp].pc_else = UINT32_MAX;
            ctl[csp].block_idx = pc;
            ctl[csp].is_loop = 0;
            ctl[csp].is_try = 1;
            ctl[csp].try_in = in;
            csp++;
            pc++;
            break;
        }
        case EA_OP_THROW: {
            uint32_t ti = in->imm.u32;
            EaTagInst *tag = &inst->tags[ti];
            EaFuncType *ft = tag->type;
            EaExnInst *exn = (EaExnInst *)ea_malloc(sizeof(EaExnInst) +
                (ft->n_params ? ft->n_params : 1) * sizeof(WVal));
            exn->tag = tag;
            exn->n_vals = ft->n_params;
            for (uint32_t q = ft->n_params; q > 0; q--) exn->vals[q - 1] = S[--sp];
            if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, exn) != NULL) {
                ex->pending_exn = exn;
                result = 1;
                goto done_err;
            }
            break;
        }
        case EA_OP_THROW_REF: {
            EaExnInst *exn = (EaExnInst *)S[--sp].ref;
            if (exn == NULL) TRAP(TRAP_NULL_REF);
            if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, exn) != NULL) {
                ex->pending_exn = exn;
                result = 1;
                goto done_err;
            }
            break;
        }
        case EA_OP_IF: {
            uint32_t cond = S[--sp].i32;
            CTL_PUSH();
            ctl[csp].height = in->height + hoff;
            ctl[csp].arity = in->arity_out;
            ctl[csp].rarity = in->arity_res;
            ctl[csp].pc_end = in->end_idx;
            ctl[csp].pc_else = in->else_idx;
            ctl[csp].block_idx = pc;
            ctl[csp].is_loop = 0;
            ctl[csp].is_try = 0;
            ctl[csp].try_in = NULL;
            csp++;
            if (!cond) {
                pc = (in->else_idx != UINT32_MAX) ? in->else_idx + 1 : in->end_idx;
            } else {
                pc++;
            }
            break;
        }
        case EA_OP_ELSE:
            // taken branch finished: jump to end
            sp = ctl[csp - 1].height + ctl[csp - 1].rarity;
            pc = ctl[csp - 1].pc_end;
            break;
        case EA_OP_END:
            if (csp > 1) {
                sp = ctl[csp - 1].height + ctl[csp - 1].rarity;
                csp--;
            } else {
                csp = 0; // function-final end: results stay on stack for the tail
            }
            pc++;
            break;
        case EA_OP_BR: {
            uint32_t l = in->imm.u32;
            RCtl *t = &ctl[csp - 1 - l];
            uint32_t arity = t->arity;
            uint32_t h = t->height;
            if (arity && h != sp - arity) memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
            sp = h + arity;
            pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
            csp -= l;
            break;
        }
        case EA_OP_BR_IF: {
            uint32_t cond = S[--sp].i32;
            if (cond) {
                uint32_t l = in->imm.u32;
                RCtl *t = &ctl[csp - 1 - l];
                uint32_t arity = t->arity;
                uint32_t h = t->height;
                if (arity && h != sp - arity) memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
                sp = h + arity;
                pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
                csp -= l;
            } else {
                pc++;
            }
            break;
        }
        case EA_OP_BR_TABLE: {
            uint32_t idx = S[--sp].i32;
            const uint32_t *tg = &f->code.pool[in->imm.pair.a];
            uint32_t n = in->imm.pair.b;
            uint32_t l = idx < n ? tg[idx] : tg[n];
            RCtl *t = &ctl[csp - 1 - l];
            uint32_t arity = t->arity;
            uint32_t h = t->height;
            if (arity && h != sp - arity) memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
            sp = h + arity;
            pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
            csp -= l;
            break;
        }
        case EA_OP_RETURN: {
            if (n_results && base != sp - n_results)
                memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
            sp = base + n_results;
            goto done;
        }
        case EA_OP_CALL: {
            uint32_t ci = in->imm.u32;
            EaFuncInst *callee = &inst->funcs[ci];
            if (callee->is_jit) {
                ex->sp = sp; // sync before the JIT call reads args at ex->sp
                if (ea_jit_call(ex, callee) != 0) {
                    if (ex->pending_exn) {
                        EaExnInst *px = ex->pending_exn;
                        ex->pending_exn = NULL;
                        if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, px) == NULL) break;
                        ex->pending_exn = px;
                    }
                    goto trapped;
                }
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else {
                ex->sp = sp;
                ex->depth++;
                int rc = ea_interp_exec_function(ex, callee);
                ex->depth--;
                S = ex->stack;
                sp = ex->sp;
                if (rc != 0) {
                    if (ex->pending_exn) {
                        EaExnInst *px = ex->pending_exn;
                        ex->pending_exn = NULL;
                        if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, px) == NULL) break;
                        ex->pending_exn = px;
                    }
                    result = rc; goto done_err;
                }
            }
            pc++;
            break;
        }
        case EA_OP_RETURN_CALL: {
            uint32_t ci = in->imm.u32;
            EaFuncInst *callee = &inst->funcs[ci];
            if (callee->is_jit) {
                ex->sp = sp;
                if (ea_jit_call(ex, callee) != 0) {
                    if (ex->pending_exn) {
                        EaExnInst *px = ex->pending_exn;
                        ex->pending_exn = NULL;
                        if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, px) == NULL) break;
                        ex->pending_exn = px;
                    }
                    goto trapped;
                }
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else {
                // true tail jump: recycle this frame for the interpreted callee
                uint32_t np2 = callee->type->n_params;
                if (np2 && base != sp - np2)
                    memmove(&S[base], &S[sp - np2], np2 * sizeof(WVal));
                sp = base + np2;
                ex->sp = sp;
                inst = callee->inst;
                m = inst->module;
                f = (EaFunc *)callee->code;
                ft = callee->type;
                n_params = ft->n_params;
                n_results = ft->n_results;
                base = sp - n_params;
                n_locals = f->n_locals;
                if (interp_ensure(ex, n_locals + f->max_stack + 8) != 0)
                    TRAP(TRAP_STACK_EXHAUSTED);
                for (uint32_t i2 = n_params; i2 < n_locals; i2++)
                    memset(&ex->stack[base + i2], 0, sizeof(WVal));
                ex->sp = base + n_locals;
                S = ex->stack;
                sp = ex->sp;
                hoff = base + n_locals;
                code = f->code.v;
                n_instr = f->code.n;
                ctl[0].height = base;
                ctl[0].arity = n_results;
                ctl[0].rarity = n_results;
                ctl[0].pc_end = n_instr;
                ctl[0].pc_else = UINT32_MAX;
                ctl[0].block_idx = UINT32_MAX;
                ctl[0].is_loop = 0;
                csp = 1;
                pc = 0;
                continue;
            }
            // jit / host callees: callee results become this frame's results
            if (n_results && base != sp - n_results)
                memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
            sp = base + n_results;
            goto done;
        }
        case EA_OP_CALL_INDIRECT: {
            uint32_t ti = in->imm.pair.b;
            EaTableInst *tab = &inst->tables[ti];
            uint64_t eidx = tab->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (eidx >= tab->size) TRAP(TRAP_UNDEF_ELEM);
            uint32_t idx = (uint32_t)eidx;
            EaFuncInst *callee = (EaFuncInst *)tab->elems[idx].ref;
            if (!callee) {
                ex->trap = TRAP_UNINIT_ELEM;
                snprintf(ex->trap_msg, sizeof(ex->trap_msg), "uninitialized element %u", idx);
                if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1);
                TRAP(TRAP_UNINIT_ELEM);
            }
            EaFuncType *want = &m->types[in->imm.pair.a].func;
            bool sig_ok;
            if (getenv("EA_GDBG")) fprintf(stderr, "ICHECK callee ti=%u want=%u\n", callee->type_idx, in->imm.pair.a);
            if (callee->inst && callee->inst->module == m)
                sig_ok = ea_type_sub(m, callee->type_idx, in->imm.pair.a, 0);
            else
                sig_ok = callee->type->n_params == want->n_params &&
                         callee->type->n_results == want->n_results &&
                         memcmp(callee->type->params, want->params, want->n_params * sizeof(EaValType)) == 0 &&
                         memcmp(callee->type->results, want->results, want->n_results * sizeof(EaValType)) == 0;
            if (!sig_ok)
                TRAP(TRAP_INDIRECT_TYPE);
            if (callee->is_jit) {
                ex->sp = sp; // sync before the JIT call reads args at ex->sp
                if (ea_jit_call(ex, callee) != 0) {
                    if (ex->pending_exn) {
                        EaExnInst *px = ex->pending_exn;
                        ex->pending_exn = NULL;
                        if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, px) == NULL) break;
                        ex->pending_exn = px;
                    }
                    goto trapped;
                }
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else {
                ex->sp = sp;
                ex->depth++;
                int rc = ea_interp_exec_function(ex, callee);
                ex->depth--;
                S = ex->stack;
                sp = ex->sp;
                if (rc != 0) {
                    if (ex->pending_exn) {
                        EaExnInst *px = ex->pending_exn;
                        ex->pending_exn = NULL;
                        if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, px) == NULL) break;
                        ex->pending_exn = px;
                    }
                    result = rc; goto done_err;
                }
            }
            pc++;
            break;
        }
        case EA_OP_RETURN_CALL_INDIRECT: {
            uint32_t ti = in->imm.pair.b;
            EaTableInst *tab = &inst->tables[ti];
            uint64_t eidx = tab->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (eidx >= tab->size) TRAP(TRAP_UNDEF_ELEM);
            uint32_t idx = (uint32_t)eidx;
            EaFuncInst *callee = (EaFuncInst *)tab->elems[idx].ref;
            if (!callee) {
                ex->trap = TRAP_UNINIT_ELEM;
                snprintf(ex->trap_msg, sizeof(ex->trap_msg), "uninitialized element %u", idx);
                if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1);
                TRAP(TRAP_UNINIT_ELEM);
            }
            EaFuncType *want = &m->types[in->imm.pair.a].func;
            bool sig_ok;
            if (getenv("EA_GDBG")) fprintf(stderr, "ICHECK callee ti=%u want=%u\n", callee->type_idx, in->imm.pair.a);
            if (callee->inst && callee->inst->module == m)
                sig_ok = ea_type_sub(m, callee->type_idx, in->imm.pair.a, 0);
            else
                sig_ok = callee->type->n_params == want->n_params &&
                         callee->type->n_results == want->n_results &&
                         memcmp(callee->type->params, want->params, want->n_params * sizeof(EaValType)) == 0 &&
                         memcmp(callee->type->results, want->results, want->n_results * sizeof(EaValType)) == 0;
            if (!sig_ok)
                TRAP(TRAP_INDIRECT_TYPE);
            if (callee->is_jit) {
                ex->sp = sp;
                if (ea_jit_call(ex, callee) != 0) {
                    if (ex->pending_exn) {
                        EaExnInst *px = ex->pending_exn;
                        ex->pending_exn = NULL;
                        if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, px) == NULL) break;
                        ex->pending_exn = px;
                    }
                    goto trapped;
                }
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else {
                // true tail jump: recycle this frame for the interpreted callee
                uint32_t np2 = callee->type->n_params;
                if (np2 && base != sp - np2)
                    memmove(&S[base], &S[sp - np2], np2 * sizeof(WVal));
                sp = base + np2;
                ex->sp = sp;
                inst = callee->inst;
                m = inst->module;
                f = (EaFunc *)callee->code;
                ft = callee->type;
                n_params = ft->n_params;
                n_results = ft->n_results;
                base = sp - n_params;
                n_locals = f->n_locals;
                if (interp_ensure(ex, n_locals + f->max_stack + 8) != 0)
                    TRAP(TRAP_STACK_EXHAUSTED);
                for (uint32_t i2 = n_params; i2 < n_locals; i2++)
                    memset(&ex->stack[base + i2], 0, sizeof(WVal));
                ex->sp = base + n_locals;
                S = ex->stack;
                sp = ex->sp;
                hoff = base + n_locals;
                code = f->code.v;
                n_instr = f->code.n;
                ctl[0].height = base;
                ctl[0].arity = n_results;
                ctl[0].rarity = n_results;
                ctl[0].pc_end = n_instr;
                ctl[0].pc_else = UINT32_MAX;
                ctl[0].block_idx = UINT32_MAX;
                ctl[0].is_loop = 0;
                csp = 1;
                pc = 0;
                continue;
            }
            if (n_results && base != sp - n_results)
                memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
            sp = base + n_results;
            goto done;
        }
        case EA_OP_REF_AS_NON_NULL:
            if (S[sp - 1].ref == NULL)
                TRAP(TRAP_NULL_DEREF);
            pc++; break;
        case EA_OP_BR_ON_NULL: {
            uint32_t l = in->imm.u32;
            RCtl *t = &ctl[csp - 1 - l];
            if (S[sp - 1].ref == NULL) {
                sp--; // drop the null ref; branch carries [t*]
                uint32_t arity = t->arity, h = t->height;
                if (arity && h != sp - arity)
                    memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
                sp = h + arity;
                pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
                csp -= l;
            } else pc++;
            break;
        }
        case EA_OP_BR_ON_NON_NULL: {
            uint32_t l = in->imm.u32;
            RCtl *t = &ctl[csp - 1 - l];
            if (S[sp - 1].ref != NULL) {
                // branch carries [t*, ref] = the full label arity
                uint32_t arity = t->arity, h = t->height;
                if (arity && h != sp - arity)
                    memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
                sp = h + arity;
                pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
                csp -= l;
            } else { sp--; pc++; }
            break;
        }
        case EA_OP_CALL_REF: case EA_OP_RETURN_CALL_REF: {
            WVal rv = S[--sp]; // the funcref sits on top of the args
            EaFuncInst *callee = (EaFuncInst *)rv.ref;
            if (callee == NULL)
                TRAP(TRAP_NULL_FUNC_REF);
            bool tail = in->opcode == EA_OP_RETURN_CALL_REF;
            if (callee->is_jit) {
                ex->sp = sp;
                if (ea_jit_call(ex, callee) != 0) {
                    if (ex->pending_exn) {
                        EaExnInst *px = ex->pending_exn;
                        ex->pending_exn = NULL;
                        if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, px) == NULL) break;
                        ex->pending_exn = px;
                    }
                    goto trapped;
                }
                S = ex->stack; sp = ex->sp;
            } else if (callee->is_host) {
                uint32_t np = callee->type->n_params;
                WVal la[16], lr[16];
                for (uint32_t i = 0; i < np && i < 16; i++) la[i] = S[sp - np + i];
                sp -= np;
                memset(lr, 0, sizeof(lr));
                if (callee->host_fn(callee->host_user, la, lr) != 0)
                    TRAP(TRAP_HOST);
                uint32_t nr = callee->type->n_results;
                for (uint32_t i = 0; i < nr; i++) S[sp + i] = lr[i];
                sp += nr;
            } else if (!tail) {
                ex->sp = sp;
                ex->depth++;
                int rc = ea_interp_exec_function(ex, callee);
                ex->depth--;
                S = ex->stack;
                sp = ex->sp;
                if (rc != 0) {
                    if (ex->pending_exn) {
                        EaExnInst *px = ex->pending_exn;
                        ex->pending_exn = NULL;
                        if (eh_unwind(ex, inst, ctl, &csp, S, &sp, &pc, px) == NULL) goto resumed;
                        ex->pending_exn = px;
                    }
                    result = rc; goto done_err;
                }
            } else {
                // true tail jump: recycle this frame for the interpreted callee
                uint32_t np2 = callee->type->n_params;
                if (np2 && base != sp - np2)
                    memmove(&S[base], &S[sp - np2], np2 * sizeof(WVal));
                sp = base + np2;
                ex->sp = sp;
                inst = callee->inst;
                m = inst->module;
                f = (EaFunc *)callee->code;
                ft = callee->type;
                n_params = ft->n_params;
                n_results = ft->n_results;
                base = sp - n_params;
                n_locals = f->n_locals;
                if (interp_ensure(ex, n_locals + f->max_stack + 8) != 0)
                    TRAP(TRAP_STACK_EXHAUSTED);
                for (uint32_t i2 = n_params; i2 < n_locals; i2++)
                    memset(&ex->stack[base + i2], 0, sizeof(WVal));
                ex->sp = base + n_locals;
                S = ex->stack;
                sp = ex->sp;
                hoff = base + n_locals;
                code = f->code.v;
                n_instr = f->code.n;
                ctl[0].height = base;
                ctl[0].arity = n_results;
                ctl[0].rarity = n_results;
                ctl[0].pc_end = n_instr;
                ctl[0].pc_else = UINT32_MAX;
                ctl[0].block_idx = UINT32_MAX;
                ctl[0].is_loop = 0;
                csp = 1;
                pc = 0;
                continue;
            }
        resumed:
            if (tail) {
                if (n_results && base != sp - n_results)
                    memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
                sp = base + n_results;
                goto done;
            }
            pc++;
            break;
        }
        case EA_OP_DROP: sp--; pc++; break;
        case EA_OP_SELECT: {
            uint32_t c = S[--sp].i32;
            WVal b = S[--sp];
            WVal a = S[--sp];
            S[sp++] = c ? a : b;
            pc++;
            break;
        }
        case EA_OP_SELECT_T: {
            uint32_t c = S[--sp].i32;
            WVal b = S[--sp];
            WVal a = S[--sp];
            S[sp++] = c ? a : b;
            pc++;
            break;
        }
        case EA_OP_LOCAL_GET: S[sp++] = S[base + in->imm.u32]; pc++; break;
        case EA_OP_LOCAL_SET: S[base + in->imm.u32] = S[--sp]; pc++; break;
        case EA_OP_LOCAL_TEE: S[base + in->imm.u32] = S[sp - 1]; pc++; break;
        case EA_OP_GLOBAL_GET: S[sp++] = *inst->globals[in->imm.u32]; pc++; break;
        case EA_OP_GLOBAL_SET: *inst->globals[in->imm.u32] = S[--sp]; pc++; break;
        case EA_OP_TABLE_GET: {
            EaTableInst *t = &inst->tables[in->imm.u32];
            uint64_t i = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (i >= t->size) TRAP(TRAP_OOB_TABLE);
            S[sp++] = t->elems[i];
            pc++;
            break;
        }
        case EA_OP_TABLE_SET: {
            EaTableInst *t = &inst->tables[in->imm.u32];
            WVal v = S[--sp];
            uint64_t i = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (i >= t->size) TRAP(TRAP_OOB_TABLE);
            t->elems[i] = v;
            pc++;
            break;
        }
        case EA_OP_TABLE_SIZE: {
            { EaTableInst *t = &inst->tables[in->imm.u32];
              if (t->is64) S[sp++].i64 = t->size; else S[sp++].i32 = (uint32_t)t->size; }
            pc++;
            break;
        }
        case EA_OP_TABLE_GROW: {
            EaTableInst *t = &inst->tables[in->imm.u32];
            uint64_t delta = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            WVal init = S[--sp];
            uint64_t old = t->size;
            uint64_t newsz = old + delta;
            if (newsz > t->max || newsz > (1ull << 32)) {
                if (t->is64) S[sp++].i64 = -1; else S[sp++].i32 = 0xFFFFFFFFu;
            } else {
                t->elems = (WVal *)ea_realloc(t->elems, (newsz ? newsz : 1) * sizeof(WVal));
                for (uint64_t i = old; i < newsz; i++) t->elems[i] = init;
                t->size = newsz;
                if (t->is64) S[sp++].i64 = old; else S[sp++].i32 = (uint32_t)old;
            }
            pc++;
            break;
        }
        case EA_OP_TABLE_FILL: {
            EaTableInst *t = &inst->tables[in->imm.u32];
            uint64_t n = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            WVal v = S[--sp];
            uint64_t d = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (d + n > t->size) TRAP(TRAP_OOB_TABLE);
            for (uint64_t i = 0; i < n; i++) t->elems[d + i] = v;
            pc++;
            break;
        }
        case EA_OP_TABLE_COPY: {
            uint32_t dt = in->imm.pair.a, st = in->imm.pair.b;
            EaTableInst *td = &inst->tables[dt], *ts = &inst->tables[st];
            // validated as [dst_idx src_idx min(dst,src)]: n is i64 only when both tables are 64-bit
            uint64_t n = (td->is64 && ts->is64) ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t s2 = ts->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t d = td->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (d + n > td->size || s2 + n > ts->size) TRAP(TRAP_OOB_TABLE);
            memmove(&td->elems[d], &ts->elems[s2], (size_t)n * sizeof(WVal));
            pc++;
            break;
        }
        case EA_OP_TABLE_INIT: {
            uint32_t ti = in->imm.pair.a, ei = in->imm.pair.b;
            EaTableInst *t = &inst->tables[ti];
            EaElem *e = &m->elems[ei];
            uint64_t n = (uint64_t)(uint32_t)S[--sp].i32; // len is always i32
            uint64_t s2 = (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t d = t->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            {
                uint64_t ilen = inst->elem_alive[in->imm.pair.b] ? e->n_items : 0;
                if (s2 + n > ilen) TRAP(TRAP_OOB_TABLE);
            }
            if (d + n > t->size) TRAP(TRAP_OOB_TABLE);
            for (uint32_t i = 0; i < n; i++) {
                WVal v;
                if (e->cache) {
                    // items were evaluated once at instantiation — reference
                    // identity must be stable across repeated table.init
                    v = e->cache[s2 + i];
                } else if (e->items) {
                    if (ea_eval_const_expr(inst, m, &e->items[s2 + i], &v, e->ref_type) != 0)
                        TRAP(TRAP_OOB_TABLE);
                } else {
                    v.ref = &inst->funcs[e->func_idx[s2 + i]];
                }
                t->elems[d + i] = v;
            }
            pc++;
            break;
        }
        case EA_OP_ELEM_DROP:
            inst->elem_alive[in->imm.u32] = 0;
            pc++;
            break;
        // ---------------- loads
        case EA_OP_I32_LOAD: case EA_OP_I64_LOAD: case EA_OP_F32_LOAD: case EA_OP_F64_LOAD:
        case EA_OP_I32_LOAD8_S: case EA_OP_I32_LOAD8_U:
        case EA_OP_I32_LOAD16_S: case EA_OP_I32_LOAD16_U:
        case EA_OP_I64_LOAD8_S: case EA_OP_I64_LOAD8_U:
        case EA_OP_I64_LOAD16_S: case EA_OP_I64_LOAD16_U:
        case EA_OP_I64_LOAD32_S: case EA_OP_I64_LOAD32_U: {
            EaMemInst *mem = inst->memories[in->imm.ma.memidx];
            uint64_t ea = (mem->is64 ? (uint64_t)S[--sp].i64
                          : (uint64_t)(uint32_t)S[--sp].i32) + in->imm.ma.offset;
            uint64_t w = load_width_bytes(op);
            if (getenv("EA_LDBG")) fprintf(stderr, "LOAD op=%x ea=%llu w=%llu size=%llu\n", op, (unsigned long long)ea, (unsigned long long)w, (unsigned long long)mem->size);
            if (w > mem->size || ea > mem->size - w) TRAP(TRAP_OOB_MEMORY);
            uint8_t *p = mem->base + ea;
            switch (op) {
            case EA_OP_I32_LOAD: memcpy(&S[sp].i32, p, 4); break;
            case EA_OP_I64_LOAD: memcpy(&S[sp].i64, p, 8); break;
            case EA_OP_F32_LOAD: memcpy(&S[sp].f32, p, 4); break;
            case EA_OP_F64_LOAD: memcpy(&S[sp].f64, p, 8); break;
            case EA_OP_I32_LOAD8_S: S[sp].i32 = (uint32_t)(int32_t)(int8_t)*p; break;
            case EA_OP_I32_LOAD8_U: S[sp].i32 = *p; break;
            case EA_OP_I32_LOAD16_S: { int16_t v; memcpy(&v, p, 2); S[sp].i32 = (uint32_t)(int32_t)v; break; }
            case EA_OP_I32_LOAD16_U: { uint16_t v; memcpy(&v, p, 2); S[sp].i32 = v; break; }
            case EA_OP_I64_LOAD8_S: S[sp].i64 = (uint64_t)(int64_t)(int8_t)*p; break;
            case EA_OP_I64_LOAD8_U: S[sp].i64 = *p; break;
            case EA_OP_I64_LOAD16_S: { int16_t v; memcpy(&v, p, 2); S[sp].i64 = (uint64_t)(int64_t)v; break; }
            case EA_OP_I64_LOAD16_U: { uint16_t v; memcpy(&v, p, 2); S[sp].i64 = v; break; }
            case EA_OP_I64_LOAD32_S: { int32_t v; memcpy(&v, p, 4); S[sp].i64 = (uint64_t)(int64_t)v; break; }
            case EA_OP_I64_LOAD32_U: { uint32_t v; memcpy(&v, p, 4); S[sp].i64 = v; break; }
            }
            sp++;
            pc++;
            break;
        }
        // ---------------- stores
        case EA_OP_I32_STORE: case EA_OP_I64_STORE: case EA_OP_F32_STORE: case EA_OP_F64_STORE:
        case EA_OP_I32_STORE8: case EA_OP_I32_STORE16:
        case EA_OP_I64_STORE8: case EA_OP_I64_STORE16: case EA_OP_I64_STORE32: {
            EaMemInst *mem = inst->memories[in->imm.ma.memidx];
            WVal v = S[--sp];
            uint64_t ea = (mem->is64 ? (uint64_t)S[--sp].i64
                          : (uint64_t)(uint32_t)S[--sp].i32) + in->imm.ma.offset;
            uint64_t w = store_width_bytes(op);
            if (w > mem->size || ea > mem->size - w) TRAP(TRAP_OOB_MEMORY);
            uint8_t *p = mem->base + ea;
            switch (op) {
            case EA_OP_I32_STORE: memcpy(p, &v.i32, 4); break;
            case EA_OP_I64_STORE: memcpy(p, &v.i64, 8); break;
            case EA_OP_F32_STORE: memcpy(p, &v.f32, 4); break;
            case EA_OP_F64_STORE: memcpy(p, &v.f64, 8); break;
            case EA_OP_I32_STORE8: case EA_OP_I64_STORE8: *p = (uint8_t)v.i32; break;
            case EA_OP_I32_STORE16: case EA_OP_I64_STORE16: { uint16_t x = (uint16_t)v.i32; memcpy(p, &x, 2); break; }
            case EA_OP_I64_STORE32: { uint32_t x = (uint32_t)v.i64; memcpy(p, &x, 4); break; }
            }
            pc++;
            break;
        }
        case EA_OP_MEMORY_SIZE: {
            EaMemInst *mem = inst->memories[in->imm.u32];
            if (mem->is64) S[sp++].i64 = mem->pages; else S[sp++].i32 = (uint32_t)mem->pages;
            pc++;
            break;
        }
        case EA_OP_MEMORY_GROW: {
            EaMemInst *mem = inst->memories[in->imm.u32];
            uint64_t delta = mem->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t old;
            if (ea_grow_memory(mem, delta, &old)) {
                if (mem->is64) S[sp++].i64 = old; else S[sp++].i32 = (uint32_t)old;
            } else {
                if (mem->is64) S[sp++].i64 = -1; else S[sp++].i32 = 0xFFFFFFFFu;
            }
            pc++;
            break;
        }
        case EA_OP_MEMORY_INIT: {
            EaMemInst *mem = inst->memories[in->imm.pair.a];
            EaData *d = &m->datas[in->imm.pair.b];
            uint64_t n = mem->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t s2 = (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t d2 = mem->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            {
                uint64_t dlen = inst->data_alive[in->imm.pair.b] ? d->data_len : 0;
                if (s2 + n > dlen) TRAP(TRAP_OOB_MEMORY);
            }
            if (n > mem->size || d2 > mem->size - n) TRAP(TRAP_OOB_MEMORY);
            if (n) memcpy(mem->base + d2, m->owned_bytes + d->data_off + s2, n);
            pc++;
            break;
        }
        case EA_OP_DATA_DROP:
            inst->data_alive[in->imm.u32] = 0;
            pc++;
            break;
        case EA_OP_MEMORY_COPY: {
            EaMemInst *dst = inst->memories[in->imm.pair.a];
            EaMemInst *src = inst->memories[in->imm.pair.b];
            // validated as [dst_idx src_idx min(dst,src)]
            uint64_t n = (dst->is64 && src->is64) ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t s2 = src->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint64_t d2 = dst->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (n > src->size || s2 > src->size - n) TRAP(TRAP_OOB_MEMORY);
            if (n > dst->size || d2 > dst->size - n) TRAP(TRAP_OOB_MEMORY);
            if (n) memmove(dst->base + d2, src->base + s2, n);
            pc++;
            break;
        }
        case EA_OP_MEMORY_FILL: {
            EaMemInst *mem = inst->memories[in->imm.u32];
            uint64_t n = mem->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            uint8_t b = (uint8_t)S[--sp].i32;
            uint64_t d2 = mem->is64 ? (uint64_t)S[--sp].i64 : (uint64_t)(uint32_t)S[--sp].i32;
            if (d2 > mem->size || n > mem->size - d2) TRAP(TRAP_OOB_MEMORY);
            if (n) memset(mem->base + d2, b, n);
            pc++;
            break;
        }
        case EA_OP_I32_CONST: S[sp++].i32 = in->imm.u32; pc++; break;
        case EA_OP_I64_CONST: S[sp++].i64 = in->imm.u64; pc++; break;
        case EA_OP_F32_CONST: S[sp++].f32 = in->imm.f32; pc++; break;
        case EA_OP_F64_CONST: S[sp++].f64 = in->imm.f64; pc++; break;
        case EA_OP_REF_NULL: S[sp++].ref = NULL; pc++; break;
        case EA_OP_REF_IS_NULL: sp--; S[sp].i32 = (S[sp].ref == NULL); sp++; pc++; break;
        case EA_OP_REF_FUNC: S[sp++].ref = &inst->funcs[in->imm.u32]; pc++; break;
        case EA_OP_REF_EQ: {
            void *b = S[--sp].ref;
            void *a = S[--sp].ref;
            S[sp++] = (WVal){.i32 = a == b};
            pc++;
            break;
        }
        // ---------------- GC (0xFB) ----------------
        case EA_OP_STRUCT_NEW: case EA_OP_STRUCT_NEW_DEFAULT: {
            uint32_t ti = in->imm.u32;
            EaType *t = &m->types[ti];
            EaHeapObj *o = ea_gc_alloc(m, ti, t->n_fields);
            if (op == EA_OP_STRUCT_NEW) {
                for (uint32_t i = t->n_fields; i > 0; i--)
                    o->data[i - 1] = EA_GC_PACK(S[--sp], t->fields[i - 1].packed_);
            }
            S[sp++].ref = o;
            pc++;
            break;
        }
        case EA_OP_STRUCT_GET: case EA_OP_STRUCT_GET_S: case EA_OP_STRUCT_GET_U: {
            uint32_t ti = in->imm.pair.a, fld = in->imm.pair.b;
            EaHeapObj *o = (EaHeapObj *)S[--sp].ref;
            if (o == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null structure reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            WVal v = o->data[fld];
            if (op == EA_OP_STRUCT_GET) S[sp++] = v;
            else if (op == EA_OP_STRUCT_GET_S) {
                S[sp].i32 = m->types[ti].fields[fld].packed_ == 1
                                ? (uint32_t)(int32_t)(int8_t)v.i32
                                : (uint32_t)(int32_t)(int16_t)v.i32;
                sp++;
            } else {
                S[sp].i32 = m->types[ti].fields[fld].packed_ == 1
                                ? (uint32_t)(uint8_t)v.i32
                                : (uint32_t)(uint16_t)v.i32;
                sp++;
            }
            pc++;
            break;
        }
        case EA_OP_STRUCT_SET: {
            uint32_t ti = in->imm.pair.a, fld = in->imm.pair.b;
            WVal v = S[--sp];
            EaHeapObj *o = (EaHeapObj *)S[--sp].ref;
            if (o == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null structure reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            o->data[fld] = EA_GC_PACK(v, m->types[ti].fields[fld].packed_);
            pc++;
            break;
        }
        case EA_OP_ARRAY_NEW: {
            uint32_t ti = in->imm.u32;
            uint32_t len = (uint32_t)S[--sp].i32;
            WVal init = S[--sp];
            EaHeapObj *o = ea_gc_alloc(m, ti, len);
            for (uint32_t i = 0; i < len; i++)
                o->data[i] = EA_GC_PACK(init, m->types[ti].fields[0].packed_);
            S[sp++].ref = o;
            pc++;
            break;
        }
        case EA_OP_ARRAY_NEW_DEFAULT: {
            uint32_t ti = in->imm.u32;
            uint32_t len = (uint32_t)S[--sp].i32;
            S[sp++].ref = ea_gc_alloc(m, ti, len);
            pc++;
            break;
        }
        case EA_OP_ARRAY_NEW_FIXED: {
            uint32_t ti = in->imm.pair.a, n = in->imm.pair.b;
            EaHeapObj *o = ea_gc_alloc(m, ti, n);
            for (uint32_t i = n; i > 0; i--)
                o->data[i - 1] = EA_GC_PACK(S[--sp], m->types[ti].fields[0].packed_);
            S[sp++].ref = o;
            pc++;
            break;
        }
        case EA_OP_ARRAY_NEW_DATA: {
            uint32_t ti = in->imm.pair.a, di = in->imm.pair.b;
            uint32_t len = (uint32_t)S[--sp].i32;
            uint32_t off = (uint32_t)S[--sp].i32;
            EaData *d = &m->datas[di];
            EaFieldType *aelf = &m->types[ti].fields[0];
            uint32_t width = aelf->packed_ == 1 ? 1 : aelf->packed_ == 2 ? 2
                           : aelf->vt == VT_I64 || aelf->vt == VT_F64 ? 8
                           : aelf->vt == VT_V128 ? 16 : 4;
            uint64_t dlen = inst->data_alive[di] ? d->data_len : 0;
            if ((uint64_t)off + len * width > dlen) TRAP(TRAP_OOB_MEMORY);
            EaHeapObj *o = ea_gc_alloc(m, ti, len);
            const uint8_t *p = m->owned_bytes + d->data_off + off;
            for (uint32_t i = 0; i < len; i++) {
                uint64_t x = 0;
                memcpy(&x, p + i * width, width < 8 ? width : 8);
                o->data[i].i32 = x;
            }
            S[sp++].ref = o;
            pc++;
            break;
        }
        case EA_OP_ARRAY_NEW_ELEM: {
            uint32_t ti = in->imm.pair.a, ei = in->imm.pair.b;
            uint32_t len = (uint32_t)S[--sp].i32;
            uint32_t off = (uint32_t)S[--sp].i32;
            EaElem *e = &m->elems[ei];
            uint64_t ilen = inst->elem_alive[ei] ? e->n_items : 0;
            if ((uint64_t)off + len > ilen) { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds table access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
            EaHeapObj *o = ea_gc_alloc(m, ti, len);
            for (uint32_t i = 0; i < len; i++) {
                WVal v;
                if (e->cache) {
                    v = e->cache[off + i];
                } else if (e->items) {
                    if (ea_eval_const_expr(inst, m, &e->items[off + i], &v, e->ref_type) != 0)
                        { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds array access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
                } else {
                    v.ref = &inst->funcs[e->func_idx[off + i]];
                }
                o->data[i] = v;
            }
            S[sp++].ref = o;
            pc++;
            break;
        }
        case EA_OP_ARRAY_GET: case EA_OP_ARRAY_GET_S: case EA_OP_ARRAY_GET_U: {
            uint32_t ti = in->imm.u32;
            uint32_t idx = (uint32_t)S[--sp].i32;
            EaHeapObj *o = (EaHeapObj *)S[--sp].ref;
            if (o == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null array reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            if (idx >= o->length) { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds array access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
            WVal v = o->data[idx];
            if (op == EA_OP_ARRAY_GET) S[sp++] = v;
            else if (op == EA_OP_ARRAY_GET_S) {
                S[sp].i32 = m->types[ti].fields[0].packed_ == 1
                                ? (uint32_t)(int32_t)(int8_t)v.i32
                                : (uint32_t)(int32_t)(int16_t)v.i32;
                sp++;
            } else {
                S[sp].i32 = m->types[ti].fields[0].packed_ == 1
                                ? (uint32_t)(uint8_t)v.i32
                                : (uint32_t)(uint16_t)v.i32;
                sp++;
            }
            pc++;
            break;
        }
        case EA_OP_ARRAY_SET: {
            uint32_t ti = in->imm.u32;
            WVal v = S[--sp];
            uint32_t idx = (uint32_t)S[--sp].i32;
            EaHeapObj *o = (EaHeapObj *)S[--sp].ref;
            if (o == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null array reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            if (idx >= o->length) { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds array access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
            o->data[idx] = EA_GC_PACK(v, m->types[ti].fields[0].packed_);
            pc++;
            break;
        }
        case EA_OP_ARRAY_LEN: {
            EaHeapObj *o = (EaHeapObj *)S[--sp].ref;
            if (o == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null array reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            if (!ea_gc_is_heap(o)) TRAP(TRAP_CAST);
            S[sp++] = (WVal){.i32 = o->length};
            pc++;
            break;
        }
        case EA_OP_ARRAY_FILL: {
            uint32_t ti = in->imm.u32;
            uint32_t len = (uint32_t)S[--sp].i32;
            WVal v = S[--sp];
            uint32_t off = (uint32_t)S[--sp].i32;
            EaHeapObj *o = (EaHeapObj *)S[--sp].ref;
            if (o == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null array reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            if ((uint64_t)off + len > o->length) { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds array access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
            for (uint32_t i = 0; i < len; i++)
                o->data[off + i] = EA_GC_PACK(v, m->types[ti].fields[0].packed_);
            pc++;
            break;
        }
        case EA_OP_ARRAY_COPY: {
            uint32_t si = in->imm.pair.b;
            uint32_t len = (uint32_t)S[--sp].i32;
            uint32_t soff = (uint32_t)S[--sp].i32;
            EaHeapObj *os = (EaHeapObj *)S[--sp].ref;
            uint32_t doff = (uint32_t)S[--sp].i32;
            EaHeapObj *od = (EaHeapObj *)S[--sp].ref;
            if (od == NULL || os == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null array reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            if ((uint64_t)doff + len > od->length || (uint64_t)soff + len > os->length)
                { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds array access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
            uint32_t dpack = m->types[in->imm.pair.a].fields[0].packed_;
            uint32_t spack = m->types[si].fields[0].packed_;
            if (dpack == spack) {
                memmove(&od->data[doff], &os->data[soff], len * sizeof(WVal));
            } else {
                for (uint32_t i = 0; i < len; i++)
                    od->data[doff + i] = EA_GC_PACK(os->data[soff + i], dpack);
            }
            (void)spack;
            pc++;
            break;
        }
        case EA_OP_ARRAY_INIT_DATA: {
            uint32_t di = in->imm.pair.b;
            uint32_t len = (uint32_t)S[--sp].i32;
            uint32_t soff = (uint32_t)S[--sp].i32;
            uint32_t off = (uint32_t)S[--sp].i32;
            EaHeapObj *o = (EaHeapObj *)S[--sp].ref;
            if (o == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null array reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            EaData *d = &m->datas[di];
            EaFieldType *aelf2 = &m->types[in->imm.pair.a].fields[0];
            uint32_t width = aelf2->packed_ == 1 ? 1 : aelf2->packed_ == 2 ? 2
                           : aelf2->vt == VT_I64 || aelf2->vt == VT_F64 ? 8
                           : aelf2->vt == VT_V128 ? 16 : 4;
            uint64_t dlen = inst->data_alive[di] ? d->data_len : 0;
            if (getenv("EA_GDBG")) fprintf(stderr, "INITDATA di=%u alive=%d dlen=%llu width=%u off=%u soff=%u len=%u\n", di, (int)inst->data_alive[di], (unsigned long long)dlen, width, off, soff, len);
            if ((uint64_t)off + len > o->length) { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds array access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
            if ((uint64_t)soff + len * width > dlen) TRAP(TRAP_OOB_MEMORY);
            const uint8_t *p = m->owned_bytes + d->data_off + soff;
            for (uint32_t i = 0; i < len; i++) {
                uint64_t x = 0;
                memcpy(&x, p + i * width, width < 8 ? width : 8);
                o->data[off + i].i32 = x;
            }
            pc++;
            break;
        }
        case EA_OP_ARRAY_INIT_ELEM: {
            uint32_t ei = in->imm.pair.b;
            uint32_t len = (uint32_t)S[--sp].i32;
            uint32_t soff = (uint32_t)S[--sp].i32;
            uint32_t off = (uint32_t)S[--sp].i32;
            EaHeapObj *o = (EaHeapObj *)S[--sp].ref;
            if (o == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null array reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            EaElem *e = &m->elems[ei];
            uint64_t ilen = inst->elem_alive[ei] ? e->n_items : 0;
            if ((uint64_t)off + len > o->length) { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds array access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
            if ((uint64_t)soff + len > ilen) { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds table access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
            for (uint32_t i = 0; i < len; i++) {
                WVal v;
                if (e->cache) {
                    v = e->cache[soff + i];
                } else if (e->items) {
                    if (ea_eval_const_expr(inst, m, &e->items[soff + i], &v, e->ref_type) != 0)
                        { ex->trap = TRAP_OOB_TABLE; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "out of bounds array access"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_OOB_TABLE); }
                } else {
                    v.ref = &inst->funcs[e->func_idx[soff + i]];
                }
                o->data[off + i] = v;
            }
            pc++;
            break;
        }
        case EA_OP_REF_I31:
            S[sp - 1].ref = ea_mk_i31((int32_t)S[sp - 1].i32);
            pc++;
            break;
        case EA_OP_I31_GET_S: case EA_OP_I31_GET_U: {
            void *r = S[--sp].ref;
            if (getenv("EA_GDBG")) fprintf(stderr, "I31GET r=%p op=%x\n", r, op);
            if (r == NULL) { ex->trap = TRAP_NULL_REF; snprintf(ex->trap_msg, sizeof(ex->trap_msg), "null i31 reference"); if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1); TRAP(TRAP_NULL_REF); }
            if (!ea_is_i31(r)) TRAP(TRAP_CAST);
            S[sp] = (WVal){.i32 = op == EA_OP_I31_GET_S ? (uint32_t)ea_i31_sval(r) : ea_i31_uval(r)};
            sp++;
            pc++;
            break;
        }
        case EA_OP_ANY_CONVERT_EXTERN: case EA_OP_EXTERN_CONVERT_ANY: {
            void *r = S[--sp].ref;
            if (r == NULL) {
                S[sp++].ref = NULL;
            } else if (op == EA_OP_EXTERN_CONVERT_ANY) {
                if ((uintptr_t)r < 4096 && !ea_is_i31(r)) {
                    // raw host values round-trip unchanged
                    S[sp++].ref = r;
                    pc++;
                    break;
                }
                // externalization is observable: wrap so ref.test (ref extern)
                // matches, keeping the original value for the round trip
                EaHeapObj *o = (EaHeapObj *)ea_malloc(sizeof(EaHeapObj) + sizeof(WVal));
                o->magic = EA_HEAP_MAGIC;
                o->kind = 4; // extern wrap
                o->type_idx = UINT32_MAX;
                o->length = 1;
                o->data[0].ref = r;
                S[sp++].ref = o;
            } else {
                // internalization: unwrap a previous externalization
                if (ea_gc_is_heap(r) && ((EaHeapObj *)r)->kind == 4)
                    r = ((EaHeapObj *)r)->data[0].ref;
                S[sp++].ref = r;
            }
            pc++;
            break;
        }
        case EA_OP_REF_TEST: case EA_OP_REF_TEST_NULL: {
            void *r = S[--sp].ref;
            bool nullable = op == EA_OP_REF_TEST_NULL;
            S[sp++] = (WVal){.i32 = ea_gc_match(m, r, (int64_t)(int32_t)in->imm.pair.a, nullable)};
            pc++;
            break;
        }
        case EA_OP_REF_CAST: case EA_OP_REF_CAST_NULL: {
            void *r = S[--sp].ref;
            bool nullable = op == EA_OP_REF_CAST_NULL;
            if (!ea_gc_match(m, r, (int64_t)(int32_t)in->imm.pair.a, nullable))
                TRAP(TRAP_CAST);
            S[sp++].ref = r;
            pc++;
            break;
        }
        case EA_OP_BR_ON_CAST: case EA_OP_BR_ON_CAST_FAIL: {
            void *r = S[sp - 1].ref;
            bool src_null = (in->imm.q.a & 1) != 0;
            bool dst_null = (in->imm.q.a & 2) != 0;
            int64_t ht1 = (int64_t)(int32_t)in->imm.q.b;
            int64_t ht2 = (int64_t)(int32_t)in->imm.q.c;
            bool cast_ok = ea_gc_match(m, r, ht2, dst_null);
            if (getenv("EA_GDBG")) fprintf(stderr, "BRONCAST r=%p ht2=%d ok=%d sp=%u arity=%u h=%u pc_end=%u loop=%d\n", r, (int)(int32_t)in->imm.q.c, cast_ok, sp, ctl[csp - 1 - in->imm.q.d].arity, ctl[csp - 1 - in->imm.q.d].height, ctl[csp - 1 - in->imm.q.d].pc_end, ctl[csp - 1 - in->imm.q.d].is_loop);
            // br_on_cast branches on a successful cast; br_on_cast_fail on a
            // failed one. The runtime value is the same on both paths — only
            // the static type differs.
            bool branch = (op == EA_OP_BR_ON_CAST) ? cast_ok : !cast_ok;
            if (branch) {
                sp--;
                S[sp++].ref = r;
                uint32_t l = in->imm.q.d;
                RCtl *t = &ctl[csp - 1 - l];
                uint32_t arity = t->arity, h = t->height;
                if (arity && h != sp - arity)
                    memmove(&S[h], &S[sp - arity], arity * sizeof(WVal));
                sp = h + arity;
                pc = t->is_loop ? t->block_idx + 1 : t->pc_end;
                csp -= l;
            } else {
                sp--;
                S[sp++].ref = r;
                pc++;
            }
            break;
        }
        default: {
            int r = exec_numeric(ex, op, S, &sp);
            if (r > 0) { pc++; break; }
            if (r < 0) goto trapped;
            r = exec_simd(ex, inst, in, S, &sp);
            if (r > 0) { pc++; break; }
            if (r < 0) goto trapped;
            TRAP(TRAP_INDIRECT_CALL);
        }
        }
    }
    // fell off the end without RETURN: function frame result
    {
        if (n_results && base != sp - n_results)
            memmove(&S[base], &S[sp - n_results], n_results * sizeof(WVal));
        sp = base + n_results;
    }
done:
    if (getenv("EA_GDBG")) fprintf(stderr, "DONE sp=%u S0=%x S1=%x result=%d\n", sp, (unsigned)S[0].i32, (unsigned)S[1].i32, result);
    ex->sp = sp;
    free(ctl);
    return result;
done_err:
    ex->sp = sp;
    free(ctl);
    return result;
trapped:
    result = 1;
    goto done_err;
#undef STACK_GUARD
}
