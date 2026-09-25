// easm runtime: store, instantiation, const-expr evaluation, traps
#include "easm.h"
#include "opcodes.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <pthread.h>

// ---------------------------------------------------------------- store
typedef struct {
    char *name;
    EaInstance *inst;
} EaNamedInst;

struct EaStore {
    EaNamedInst *named;
    uint32_t n_named, cap_named;
    EaExec exec;
};

void ea_store_set_max_depth(EaStore *s, uint32_t d) {
    s->exec.max_depth = d;
}

EaStore *ea_store_new(void) {
    EaStore *s = (EaStore *)ea_zalloc(sizeof(EaStore));
    s->exec.max_depth = 2500;
    return s;
}
void ea_store_free(EaStore *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->n_named; i++) free(s->named[i].name);
    free(s->named);
    free(s->exec.stack);
    free(s);
}
void ea_register_instance(EaStore *s, const char *name, EaInstance *inst) {
    if (s->n_named == s->cap_named) {
        s->cap_named = s->cap_named ? s->cap_named * 2 : 8;
        s->named = (EaNamedInst *)ea_realloc(s->named, s->cap_named * sizeof(EaNamedInst));
    }
    s->named[s->n_named].name = ea_strndup(name, strlen(name));
    s->named[s->n_named].inst = inst;
    s->n_named++;
}
EaInstance *ea_lookup_instance(EaStore *s, const char *name) {
    for (uint32_t i = 0; i < s->n_named; i++)
        if (strcmp(s->named[i].name, name) == 0) return s->named[i].inst;
    return NULL;
}
// remove a previously registered instance by name (used by embedders that
// register transient import sources around instantiation). No-op when missing.
void ea_store_unregister(EaStore *s, const char *name) {
    for (uint32_t i = 0; i < s->n_named; i++) {
        if (strcmp(s->named[i].name, name) == 0) {
            free(s->named[i].name);
            s->named[i] = s->named[s->n_named - 1];
            s->n_named--;
            return;
        }
    }
}

int ea_instance_export(EaInstance *inst, const char *name, uint8_t kind) {
    for (uint32_t i = 0; i < inst->module->n_exports; i++) {
        EaExport *ex = &inst->module->exports[i];
        if (ex->kind == kind && strcmp(ex->name, name) == 0) return (int)ex->idx;
    }
    return -1;
}

// length-aware lookup: export names may contain NUL bytes
int ea_instance_export_n(EaInstance *inst, const char *name, uint32_t name_len, uint8_t kind) {
    if (!name) return -1;
    for (uint32_t i = 0; i < inst->module->n_exports; i++) {
        EaExport *ex = &inst->module->exports[i];
        if (ex->kind == kind && ex->name_len == name_len &&
            memcmp(ex->name, name, name_len) == 0) return (int)ex->idx;
    }
    return -1;
}

// ---------------------------------------------------------------- trap plumbing
const char *ea_store_last_trap_msg(EaStore *s) {
    return s->exec.trap_msg[0] ? s->exec.trap_msg : ea_trap_msg(s->exec.trap);
}

// for the wast driver's assert_exception: is an uncaught wasm exception in
// flight? consumes it (clears) either way
bool ea_store_take_pending_exn(EaStore *s) {
    struct EaExnInst *px = s->exec.pending_exn;
    s->exec.pending_exn = NULL;
    return px != NULL;
}

void ea_trap(EaExec *ex, EaTrap code) {
    if (getenv("EA_TDBG")) fprintf(stderr, "TRAP %d\n", code);
    ex->trap = code;
    ex->trap_msg[0] = 0;
    if (ex->jb) longjmp(*(jmp_buf *)ex->jb, 1);
    fprintf(stderr, "easm: uncaught trap %d\n", code);
    abort();
}

// ---------------------------------------------------------------- const expr eval
int ea_eval_const_expr(EaInstance *inst, EaModule *m, const InsList *code, WVal *out,
                       EaValType expect) {
    WVal stack[16];
    (void)0;
    uint32_t sp = 0;
    for (uint32_t pc = 0; pc < code->n; pc++) {
        EaInstr *in = &code->v[pc];
        uint32_t op = in->opcode;
        switch (in->opcode) {
        case EA_OP_I32_CONST: stack[sp++].i32 = in->imm.u32; break;
        case EA_OP_I64_CONST: stack[sp++].i64 = in->imm.u64; break;
        case EA_OP_F32_CONST: stack[sp++].f32 = in->imm.f32; break;
        case EA_OP_F64_CONST: stack[sp++].f64 = in->imm.f64; break;
        case EA_OP_V128_CONST:
            memcpy(stack[sp++].v128, in->imm.bytes, 16);
            break;
        case EA_OP_GLOBAL_GET: {
            uint32_t gi = in->imm.u32;
            if (!inst || gi >= inst->n_globals) return -1;
            stack[sp++] = *inst->globals[gi];
            break;
        }
        case EA_OP_REF_NULL:
            stack[sp++].ref = NULL;
            break;
        case EA_OP_REF_FUNC: {
            if (!inst || in->imm.u32 >= inst->n_funcs) return -1;
            stack[sp++].ref = &inst->funcs[in->imm.u32];
            break;
        }
        case EA_OP_I32_ADD: sp--; stack[sp - 1].i32 += stack[sp].i32; break;
        case EA_OP_I32_SUB: sp--; stack[sp - 1].i32 -= stack[sp].i32; break;
        case EA_OP_I32_MUL: sp--; stack[sp - 1].i32 *= stack[sp].i32; break;
        case EA_OP_I64_ADD: sp--; stack[sp - 1].i64 += stack[sp].i64; break;
        case EA_OP_I64_SUB: sp--; stack[sp - 1].i64 -= stack[sp].i64; break;
        case EA_OP_I64_MUL: sp--; stack[sp - 1].i64 *= stack[sp].i64; break;
        case EA_OP_STRUCT_NEW: case EA_OP_STRUCT_NEW_DEFAULT: {
            uint32_t ti = in->imm.u32;
            EaType *t = &m->types[ti];
            EaHeapObj *o = ea_gc_alloc(m, ti, t->n_fields);
            if (op == EA_OP_STRUCT_NEW)
                for (uint32_t i = t->n_fields; i > 0; i--)
                    o->data[i - 1] = EA_GC_PACK(stack[--sp], t->fields[i - 1].packed_);
            stack[sp++].ref = o;
            break;
        }
        case EA_OP_ARRAY_NEW: case EA_OP_ARRAY_NEW_DEFAULT: {
            uint32_t ti = in->imm.u32;
            uint32_t len = (uint32_t)stack[--sp].i32;
            EaHeapObj *o = ea_gc_alloc(m, ti, len);
            if (op == EA_OP_ARRAY_NEW) {
                WVal init = stack[--sp];
                for (uint32_t i = 0; i < len; i++)
                    o->data[i] = EA_GC_PACK(init, m->types[ti].fields[0].packed_);
            }
            stack[sp++].ref = o;
            break;
        }
        case EA_OP_ARRAY_NEW_DATA: {
            uint32_t ti = in->imm.pair.a, di = in->imm.pair.b;
            uint32_t len = (uint32_t)stack[--sp].i32;
            uint32_t off = (uint32_t)stack[--sp].i32;
            if (!inst->data_alive[di]) return -1;
            EaData *d = &m->datas[di];
            EaFieldType *aelf = &m->types[ti].fields[0];
            uint32_t width = aelf->packed_ == 1 ? 1 : aelf->packed_ == 2 ? 2
                           : aelf->vt == VT_I64 || aelf->vt == VT_F64 ? 8
                           : aelf->vt == VT_V128 ? 16 : 4;
            uint64_t dlen = d->data_len;
            if ((uint64_t)off + len * width > dlen) return -1;
            EaHeapObj *o = ea_gc_alloc(m, ti, len);
            const uint8_t *p = m->owned_bytes + d->data_off + off;
            for (uint32_t i = 0; i < len; i++) {
                uint64_t x = 0;
                memcpy(&x, p + i * width, width < 8 ? width : 8);
                o->data[i].i32 = x;
            }
            stack[sp++].ref = o;
            break;
        }
        case EA_OP_ARRAY_NEW_FIXED: {
            uint32_t ti = in->imm.pair.a, n = in->imm.pair.b;
            EaHeapObj *o = ea_gc_alloc(m, ti, n);
            for (uint32_t i = n; i > 0; i--)
                o->data[i - 1] = EA_GC_PACK(stack[--sp], m->types[ti].fields[0].packed_);
            stack[sp++].ref = o;
            break;
        }
        case EA_OP_REF_I31:
            stack[sp - 1].ref = ea_mk_i31((int32_t)stack[sp - 1].i32);
            break;
        case EA_OP_ANY_CONVERT_EXTERN: case EA_OP_EXTERN_CONVERT_ANY: {
            void *r = stack[--sp].ref;
            if (op == EA_OP_EXTERN_CONVERT_ANY && r != NULL &&
                (uintptr_t)r >= 4096 && !ea_is_i31(r)) {
                EaHeapObj *o = (EaHeapObj *)ea_malloc(sizeof(EaHeapObj) + sizeof(WVal));
                o->magic = EA_HEAP_MAGIC;
                o->kind = 4;
                o->type_idx = UINT32_MAX;
                o->length = 1;
                o->data[0].ref = r;
                r = o;
            } else if (op == EA_OP_ANY_CONVERT_EXTERN && r != NULL &&
                       ea_gc_is_heap(r) && ((EaHeapObj *)r)->kind == 4) {
                r = ((EaHeapObj *)r)->data[0].ref;
            }
            stack[sp++].ref = r;
            break;
        }
        default:
            return -1;
        }
    }
    if (sp == 0) return -1;
    *out = stack[--sp];
    (void)expect;
    (void)m;
    return 0;
}

// ---------------------------------------------------------------- memory alloc
#define EA_MEM_RESERVE ((size_t)(12ull << 30)) // 12 GiB VA per memory

// ---- signal-based out-of-bounds detection
// every memory reserves 12 GiB of PROT_NONE address space, so any i32
// address (or i32 address + validated offset, both < 2^33) that escapes the
// committed pages faults INSIDE the reservation.  The SIGSEGV/SIGBUS handler
// converts such faults into wasm OOB traps; anything else is a real crash.
#include <signal.h>

typedef struct { uint8_t *base, *end; } EaMemRange;
static EaMemRange *g_mem_ranges;
static uint32_t g_n_mem_ranges, g_cap_mem_ranges;
static _Thread_local EaExec *g_fault_exec; // exec owning the active invoke

static void ea_segv_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uintptr_t addr = (uintptr_t)si->si_addr;
    EaExec *ex = g_fault_exec;
    if (getenv("EA_FDBG")) fprintf(stderr, "[F] fault addr=%p ranges=%u\n", si->si_addr, g_n_mem_ranges);
    if (ex && ex->jb) {
        for (uint32_t i = 0; i < g_n_mem_ranges; i++) {
            if (getenv("EA_FDBG")) fprintf(stderr, "[F] range[%u] [%p,%p)\n", i, g_mem_ranges[i].base, g_mem_ranges[i].end);
            if (addr >= (uintptr_t)g_mem_ranges[i].base &&
                addr < (uintptr_t)g_mem_ranges[i].end) {
                ex->trap = TRAP_OOB_MEMORY;
                snprintf(ex->trap_msg, sizeof(ex->trap_msg), "%s",
                         ea_trap_msg(TRAP_OOB_MEMORY));
                sigset_t set; // the fault stays blocked inside the handler
                sigemptyset(&set);
                sigaddset(&set, SIGSEGV);
                sigaddset(&set, SIGBUS);
                sigprocmask(SIG_UNBLOCK, &set, NULL);
                longjmp(*(jmp_buf *)ex->jb, 1);
            }
        }
    }
    signal(sig, SIG_DFL); // not a wasm memory fault: crash for real
    raise(sig);
}

static void ea_install_fault_handler(void) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ea_segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

uint8_t *alloc_memory(uint64_t pages);
uint8_t *alloc_memory_public(uint64_t pages) {
    return alloc_memory(pages);
}

// ---- embedder lifecycle helpers (JS WebAssembly.* bridge)
// Create a standalone linear memory (owned): reserves the guard region and
// tracks it for signal-based OOB, then commits `min_pages`.
EaMemInst *ea_memory_new(uint64_t min_pages, uint64_t max_pages, bool has_max, bool is64) {
    EaMemInst *mi = (EaMemInst *)ea_zalloc(sizeof(EaMemInst));
    mi->base = alloc_memory_public(min_pages);
    mi->pages = min_pages;
    mi->size = min_pages << 16;
    mi->max_pages = max_pages;
    mi->has_max = has_max;
    mi->is64 = is64;
    mi->owned = true;
    return mi;
}
void ea_memory_free(EaMemInst *mi) {
    if (!mi) return;
    if (mi->owned && mi->base) {
        for (uint32_t r = 0; r < g_n_mem_ranges; r++) {
            if (g_mem_ranges[r].base == mi->base) {
                g_mem_ranges[r] = g_mem_ranges[--g_n_mem_ranges];
                break;
            }
        }
        munmap(mi->base, EA_MEM_RESERVE);
    }
    free(mi);
}
// Create a standalone table (owned storage).
EaTableInst *ea_table_new(EaValType ref_type, uint64_t min, uint64_t max, bool has_max, bool is64) {
    EaTableInst *ti = (EaTableInst *)ea_zalloc(sizeof(EaTableInst));
    ti->ref_type = ref_type;
    ti->elems = (WVal *)ea_zalloc((size_t)(min > 0 ? min : 1) * sizeof(WVal));
    ti->size = min;
    ti->max = has_max ? max : UINT64_MAX;
    ti->has_max = has_max;
    ti->is64 = is64;
    return ti;
}
void ea_table_free(EaTableInst *ti) {
    if (!ti) return;
    free(ti->elems);
    free(ti);
}
uint8_t *alloc_memory(uint64_t pages) {
    void *p = mmap(NULL, EA_MEM_RESERVE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (pages > 0) {
        if (mprotect(p, (size_t)(pages << 16), PROT_READ | PROT_WRITE) != 0) {
            munmap(p, EA_MEM_RESERVE);
            return NULL;
        }
    }
    if (g_n_mem_ranges == g_cap_mem_ranges) {
        g_cap_mem_ranges = g_cap_mem_ranges ? g_cap_mem_ranges * 2 : 16;
        g_mem_ranges = (EaMemRange *)realloc(g_mem_ranges,
                        g_cap_mem_ranges * sizeof(EaMemRange));
    }
    g_mem_ranges[g_n_mem_ranges].base = (uint8_t *)p;
    g_mem_ranges[g_n_mem_ranges].end = (uint8_t *)p + EA_MEM_RESERVE;
    g_n_mem_ranges++;
    ea_install_fault_handler();
    return (uint8_t *)p;
}
bool ea_grow_memory(EaMemInst *mi, uint64_t delta, uint64_t *old) {
    *old = mi->pages;
    if (mi->pages + delta > mi->max_pages) return false;
    if (!mi->is64 && mi->pages + delta > 65536) return false; // 32-bit spec limit
    if (mprotect(mi->base, (size_t)((mi->pages + delta) << 16),
                 PROT_READ | PROT_WRITE) != 0)
        return false;
    mi->pages += delta;
    mi->size = mi->pages << 16;
    return true;
}

void ea_instance_free(EaInstance *inst) {
    if (!inst) return;
    for (uint32_t i = 0; i < inst->n_tables; i++) free(inst->tables[i].elems);
    for (uint32_t i = 0; i < inst->n_memories; i++) {
        // imported memories alias the source instance's storage; only
        // defined (owned) ones belong to this instance
        if (i >= inst->module->n_imp_memories && inst->memories[i] &&
            inst->memories[i]->owned) {
            if (inst->memories[i]->base) {
                for (uint32_t r = 0; r < g_n_mem_ranges; r++) {
                    if (g_mem_ranges[r].base == inst->memories[i]->base) {
                        g_mem_ranges[r] = g_mem_ranges[--g_n_mem_ranges];
                        break;
                    }
                }
                munmap(inst->memories[i]->base, EA_MEM_RESERVE);
            }
            free(inst->memories[i]);
        }
    }
    free(inst->funcs);
    free(inst->tables);
    free(inst->memories);
    free(inst->globals);
    free(inst->tags);
    free(inst->data_alive);
    free(inst->elem_alive);
    free(inst);
}

// ---------------------------------------------------------------- instantiation
static bool type_matches(EaFuncType *a, EaFuncType *b) {
    if (a->n_params != b->n_params || a->n_results != b->n_results) return false;
    for (uint32_t i = 0; i < a->n_params; i++)
        if (a->params[i] != b->params[i]) return false;
    for (uint32_t i = 0; i < a->n_results; i++)
        if (a->results[i] != b->results[i]) return false;
    return true;
}

int ea_store_instantiate(EaStore *s, EaModule *m, EaInstance **out, char **err_msg, EaTrap *trap) {
    EaInstance *inst = (EaInstance *)ea_zalloc(sizeof(EaInstance));
    inst->module = m;
    inst->store = s;
    inst->start_func = m->has_start ? m->start_func : UINT32_MAX;
    const char *fail = NULL;

    uint32_t n_funcs = m->n_funcs, n_tables = m->n_tables, n_mems = m->n_memories,
             n_globals = m->n_globals_def, n_tags = m->n_tags;
    inst->n_funcs = n_funcs;
    inst->n_tables = n_tables;
    inst->n_memories = n_mems;
    inst->n_globals = n_globals;
    inst->n_tags = n_tags;
    inst->funcs = (EaFuncInst *)ea_zalloc((n_funcs ? n_funcs : 1) * sizeof(EaFuncInst));
    inst->tables = (EaTableInst *)ea_zalloc((n_tables ? n_tables : 1) * sizeof(EaTableInst));
    inst->memories = (EaMemInst **)ea_zalloc((n_mems ? n_mems : 1) * sizeof(EaMemInst *));
    inst->globals = (WVal **)ea_zalloc((n_globals ? n_globals : 1) * sizeof(WVal *));
    inst->tags = (EaTagInst *)ea_zalloc((n_tags ? n_tags : 1) * sizeof(EaTagInst));

    uint32_t ifunc = 0, itable = 0, imem = 0, iglob = 0, itag = 0;
    for (uint32_t i = 0; i < m->n_imports; i++) {
        EaImport *im = &m->imports[i];
        EaInstance *src = ea_lookup_instance(s, im->module);
        if (!src) {
            fail = "unknown import";
            goto unlinkable;
        }
        if (getenv("EA_IDBG")) fprintf(stderr, "IMP[%u] kind=%u %s.%s\n", i, im->kind, im->module, im->name);
        switch (im->kind) {
        case EAK_FUNC: {
            int idx = ea_instance_export(src, im->name, EAK_FUNC);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            EaFuncInst *fi = &src->funcs[idx];
            // the actual func must be a subtype of the declared import type,
            // with canonical equivalence resolved across both modules; host
            // modules may lack a type space, so fall back to a structural
            // signature comparison there
            bool fok;
            if (fi->type_idx < src->module->n_types && im->idx < m->n_types)
                fok = ea_type_sub_mm(src->module, fi->type_idx, m, im->idx, 0);
            else
                fok = type_matches(fi->type, &m->types[im->idx].func);
            if (!fok) {
                if (getenv("EA_IDBG")) fprintf(stderr, "FAIL func import %s.%s host(np=%u nr=%u p=%d,%d,%d,%d r0=%d) guest(np=%u nr=%u p=%d,%d,%d,%d)\n",
                    im->module, im->name, fi->type->n_params, fi->type->n_results,
                    fi->type->n_params > 0 ? fi->type->params[0] : -1,
                    fi->type->n_params > 1 ? fi->type->params[1] : -1,
                    fi->type->n_params > 2 ? fi->type->params[2] : -1,
                    fi->type->n_params > 3 ? fi->type->params[3] : -1,
                    fi->type->n_results ? fi->type->results[0] : -1,
                    m->types[im->idx].func.n_params, m->types[im->idx].func.n_results,
                    m->types[im->idx].func.n_params > 0 ? m->types[im->idx].func.params[0] : -1,
                    m->types[im->idx].func.n_params > 1 ? m->types[im->idx].func.params[1] : -1,
                    m->types[im->idx].func.n_params > 2 ? m->types[im->idx].func.params[2] : -1,
                    m->types[im->idx].func.n_params > 3 ? m->types[im->idx].func.params[3] : -1);
                fail = "incompatible import type";
                goto unlinkable;
            }
            inst->funcs[ifunc++] = *fi;
            break;
        }
        case EAK_TABLE: {
            int idx = ea_instance_export(src, im->name, EAK_TABLE);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            EaTableInst *ti = &src->tables[idx];
            if (ti->ref_type != im->table.ref_type || ti->is64 != im->table.is64) {
                if (getenv("EA_IDBG")) fprintf(stderr, "FAIL table import reftype\n");
                fail = "incompatible import type"; goto unlinkable;
            }
            if (ti->size < im->table.min) { fail = "incompatible import type"; goto unlinkable; }
            if (im->table.has_max) {
                if (!ti->has_max || ti->max > im->table.max) {
                    fail = "incompatible import type";
                    goto unlinkable;
                }
            }
            inst->tables[itable++] = *ti;
            break;
        }
        case EAK_MEMORY: {
            int idx = ea_instance_export(src, im->name, EAK_MEMORY);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            EaMemInst *mi = src->memories[idx];
            if (mi->is64 != im->memory.is64) { fail = "incompatible import type"; goto unlinkable; }
            if (mi->pages < im->memory.min) { fail = "incompatible import type"; goto unlinkable; }
            if (im->memory.has_max) {
                uint64_t no_max_limit = mi->is64 ? (1ull << 48) : 65536;
                uint64_t src_max = mi->has_max ? mi->max_pages : no_max_limit;
                if (!mi->has_max && im->memory.max < no_max_limit) {
                    fail = "incompatible import type";
                    goto unlinkable;
                }
                if (mi->has_max && src_max > im->memory.max) {
                    fail = "incompatible import type";
                    goto unlinkable;
                }
            }
            inst->memories[imem++] = mi; // alias the source storage (shared growth)
            break;
        }
        case EAK_TAG: {
            int idx = ea_instance_export(src, im->name, EAK_TAG);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            // tag types must be canonically equivalent (both directions),
            // resolved across the two modules
            EaTag *want = &m->tags[im->idx];
            EaTag *got = &src->module->tags[idx];
            if (!ea_type_canon_eq(m, want->type_idx, src->module, got->type_idx)) {
                fail = "incompatible import type";
                goto unlinkable;
            }
            inst->tags[itag++] = src->tags[idx]; // identity (module+tag_idx) preserved
            break;
        }
        case EAK_GLOBAL: {
            int idx = ea_instance_export(src, im->name, EAK_GLOBAL);
            if (idx < 0) { fail = "unknown import"; goto unlinkable; }
            EaValType st = src->module->globals_def[idx].type;
            bool smut = src->module->globals_def[idx].mutable_;
            // mutability must match exactly; immutable imports accept any
            // subtype (covariance), mutable ones require canonical equality
            bool tok = smut == im->global.mutable_ &&
                       (smut ? ea_vt_canon_eq(m, st, im->global.type)
                             : ea_vt_sub(m, st, im->global.type, 0));
            if (!tok) {
                if (getenv("EA_IDBG")) fprintf(stderr, "FAIL global: %s.%s src.type=%d want=%d src.mut=%d want.mut=%d idx=%d n=%u\n",
                                               im->module, im->name, st, im->global.type,
                                               smut, im->global.mutable_,
                                               idx, src->module->n_globals_def);
                fail = "incompatible import type";
                goto unlinkable;
            }
            inst->globals[iglob++] = src->globals[idx]; // alias the source storage
            if (getenv("EA_IDBG")) fprintf(stderr, "IMP global %s.%s ok type=%d mut=%d\n",
                                           im->module, im->name, im->global.type, im->global.mutable_);
            break;
        }
        }
    }

    // ---- defined tags: identity
    for (uint32_t i = m->n_imp_tags; i < n_tags; i++) {
        inst->tags[i].type = &m->types[m->tags[i].type_idx].func;
        inst->tags[i].module = m;
        inst->tags[i].tag_idx = i;
        inst->tags[i].ident = &inst->tags[i];
    }
    // imported tags without a source identity (spectest-style host tags)
    for (uint32_t i = 0; i < m->n_imp_tags; i++) {
        if (inst->tags[i].module == NULL) {
            inst->tags[i].module = m;
            inst->tags[i].tag_idx = i;
        }
    }
    // ---- defined functions
    for (uint32_t i = m->n_imp_funcs; i < n_funcs; i++) {
        EaFuncInst *fi = &inst->funcs[i];
        fi->type = &m->types[m->funcs[i].type_idx].func;
        fi->type_idx = m->funcs[i].type_idx;
        fi->inst = inst;
        fi->func_idx = i;
        fi->code = &m->funcs[i];
        fi->is_jit = m->funcs[i].jit_code != NULL;
        fi->jit_entry = m->funcs[i].jit_code;
    }

    // ---- globals (imports first, then defined, in order)
    for (uint32_t i = m->n_imp_globals; i < n_globals; i++)
        inst->globals[i] = (WVal *)ea_zalloc(sizeof(WVal));
    for (uint32_t i = m->n_imp_globals; i < n_globals; i++) {
        EaGlobal *g = &m->globals_def[i];
        WVal v;
        if (ea_eval_const_expr(inst, m, &g->init, &v, g->type) != 0) {
            fail = "invalid global initializer";
            goto uninstantiable;
        }
        *inst->globals[i] = v;
    }

    // ---- tables
    for (uint32_t i = m->n_imp_tables; i < n_tables; i++) {
        EaTable *t = &m->tables[i];
        EaTableInst *ti = &inst->tables[i];
        ti->ref_type = t->ref_type;
        ti->is64 = t->is64;
        ti->max = t->has_max ? t->max : (t->is64 ? UINT64_MAX : UINT32_MAX);
        ti->has_max = t->has_max;
        ti->elems = (WVal *)ea_zalloc((t->min ? t->min : 1) * sizeof(WVal));
        ti->size = t->min;
        if (t->has_init) {
            WVal iv;
            if (ea_eval_const_expr(inst, m, &t->init, &iv, t->ref_type) != 0) {
                fail = "invalid table initializer";
                goto uninstantiable;
            }
            for (uint32_t k = 0; k < ti->size; k++) ti->elems[k] = iv;
        }
    }
    // ---- memories
    for (uint32_t i = m->n_imp_memories; i < n_mems; i++) {
        EaMemory *mm = &m->memories[i];
        EaMemInst *mi = (EaMemInst *)ea_zalloc(sizeof(EaMemInst));
        inst->memories[i] = mi;
        mi->base = alloc_memory(mm->min);
        if (!mi->base) { fail = "out of memory"; goto uninstantiable; }
        mi->pages = mm->min;
        mi->max_pages = mm->has_max ? mm->max : 65536;
        mi->has_max = mm->has_max;
        mi->size = mm->min << 16;
        mi->is64 = mm->is64;
        mi->owned = true;
    }

    inst->elem_alive = (uint8_t *)ea_zalloc(m->n_elems ? m->n_elems : 1);
    inst->data_alive = (uint8_t *)ea_zalloc(m->n_datas ? m->n_datas : 1);

    // ---- element segment item cache (evaluate exprs once for ref identity);
    // runs after funcs/globals/memories exist because item exprs may
    // reference any of them via global.get / ref.func
    for (uint32_t i = 0; i < m->n_elems; i++) {
        EaElem *e = &m->elems[i];
        if (!e->items || e->cache) continue;
        e->cache = (WVal *)ea_zalloc((e->n_items ? e->n_items : 1) * sizeof(WVal));
        for (uint32_t j = 0; j < e->n_items; j++) {
            WVal v;
            if (ea_eval_const_expr(inst, m, &e->items[j], &v, e->ref_type) != 0)
                v = (WVal){0};
            e->cache[j] = v;
        }
    }

    // JIT fast-path fields — set BEFORE element/data init and the start
    // function: an instantiation that fails partway still leaks funcinsts
    // into imported tables (spec: prior writes persist), and those must
    // stay callable from JIT frames, whose prologue loads these unconditionally
    {
        static EaMemInst g_dummy_mem; // base=NULL,size=0: any access traps
        inst->jit_mem0 = n_mems ? inst->memories[0] : &g_dummy_mem;
    }
    inst->jit_globals = inst->globals;
    // ---- element segments
    for (uint32_t i = 0; i < m->n_elems; i++) {
        EaElem *e = &m->elems[i];
        if (e->mode == SEG_DECLARATIVE) continue;
        if (e->mode == SEG_PASSIVE) {
            inst->elem_alive[i] = 1;
            continue;
        }
        uint32_t n = e->n_items;
        WVal *vals = NULL;
        if (n) vals = (WVal *)ea_malloc(n * sizeof(WVal));
        for (uint32_t j = 0; j < n; j++) {
            if (e->items) {
                if (ea_eval_const_expr(inst, m, &e->items[j], &vals[j], e->ref_type) != 0) {
                    free(vals);
                    fail = "invalid element expression";
                    goto uninstantiable;
                }
            } else {
                vals[j].ref = &inst->funcs[e->func_idx[j]];
            }
        }
        WVal offv;
        if (ea_eval_const_expr(inst, m, &e->offset, &offv, VT_I32) != 0) {
            free(vals);
            fail = "invalid element offset";
            goto uninstantiable;
        }
        uint64_t off = (uint32_t)offv.i32;
        EaTableInst *ti = &inst->tables[e->table_idx];
        if (n > ti->size || off > ti->size - n) {
            free(vals);
            fail = "out of bounds table access";
            goto uninstantiable;
        }
        for (uint32_t j = 0; j < n; j++) ti->elems[off + j] = vals[j];
        free(vals);
    }

    // ---- data segments
    for (uint32_t i = 0; i < m->n_datas; i++) {
        EaData *d = &m->datas[i];
        if (d->mode != SEG_ACTIVE) {
            inst->data_alive[i] = 1;
            continue;
        }
        WVal offv;
        if (ea_eval_const_expr(inst, m, &d->offset, &offv, VT_I32) != 0) {
            fail = "invalid data offset";
            goto uninstantiable;
        }
        uint64_t off = (uint32_t)offv.i32;
        EaMemInst *mi = inst->memories[d->mem_idx];
        if (d->data_len > mi->size || off > mi->size - d->data_len) {
            fail = "out of bounds memory access";
            goto uninstantiable;
        }
        if (d->data_len) memcpy(mi->base + off, m->owned_bytes + d->data_off, d->data_len);
    }

    *out = inst;
    if (inst->start_func != UINT32_MAX) {
        EaTrap t = TRAP_NONE;
        int r = ea_instance_invoke(s, inst, inst->start_func, NULL, NULL, &t);
        if (r != 0) {
            if (trap) *trap = t;
            if (err_msg && !*err_msg)
                *err_msg = ea_strndup("start function trapped", 22);
            if (getenv("EA_LDBG"))
                fprintf(stderr, "[LDBG] start-trap inst=%p jit_mem0=%p\n", (void *)inst, (void *)inst->jit_mem0);
            return 2;
        }
    }
    return 0;

unlinkable:
    if (err_msg && !*err_msg) *err_msg = ea_strndup(fail, strlen(fail));
    if (trap) *trap = TRAP_NONE;
    return 1;
uninstantiable:
    if (err_msg && !*err_msg) *err_msg = ea_strndup(fail, strlen(fail));
    if (trap) *trap = TRAP_OOB_MEMORY;
    return 2;
}

// ---------------------------------------------------------------- invoke
int ea_instance_invoke(EaStore *s, EaInstance *inst, uint32_t func_idx,
                       const WVal *args, WVal *results, EaTrap *trap) {
    EaExec *ex = &s->exec;
    if (func_idx >= inst->n_funcs) return -1;
    EaFuncInst *fi = &inst->funcs[func_idx];
    EaExec *saved_fault_exec = g_fault_exec;
    g_fault_exec = ex; // in-bounds faults during this invoke become traps
    ex->trap = TRAP_NONE;
    ex->pending_exn = NULL; // stale state from a previous invoke must not leak
    uint32_t eh_base = ex->eh_top; // traps longjmp past JIT handler pops
    // native stack floor for JIT'd frames: recursion that would run past the
    // thread stack traps as TRAP_STACK_EXHAUSTED instead of faulting
    {
        void *sp0 = (void *)&ex;
        size_t ssz = pthread_get_stacksize_np(pthread_self());
        ex->jit_stack_limit = (char *)sp0 - ssz + 256 * 1024;
    }
    jmp_buf jb;
    jmp_buf *saved = (jmp_buf *)ex->jb;
    ex->jb = &jb;
    int ret;
    if (setjmp(jb) == 0) {
        ex->depth = 0; // reset in case a previous call trapped mid-recursion
        uint32_t n_params = fi->type->n_params;
        if (interp_ensure(ex, n_params + 64) != 0) {
            ex->jb = saved;
            g_fault_exec = saved_fault_exec;
            if (trap) *trap = TRAP_STACK_EXHAUSTED;
            return 2;
        }
        for (uint32_t i = 0; i < n_params; i++) ex->stack[ex->sp++] = args[i];
        int rc;
        if (fi->is_jit) {
            rc = ea_jit_call(ex, fi);
        } else if (fi->is_host) {
            WVal local_args[16];
            WVal local_results[16];
            for (uint32_t i = 0; i < n_params && i < 16; i++) local_args[i] = ex->stack[ex->sp - n_params + i];
            ex->sp -= n_params;
            memset(local_results, 0, sizeof(local_results));
            int hr = fi->host_fn(fi->host_user, local_args, local_results);
            rc = hr == 0 ? 0 : 1;
            if (rc == 0) {
                uint32_t n_results = fi->type->n_results;
                if (interp_ensure(ex, n_results) == 0) {
                    for (uint32_t i = 0; i < n_results; i++) ex->stack[ex->sp++] = local_results[i];
                } else {
                    rc = 1;
                    ex->trap = TRAP_STACK_EXHAUSTED;
                }
            } else {
                ex->trap = TRAP_HOST;
            }
        } else {
            rc = ea_interp_exec_function(ex, fi);
        }
        if (rc == 0 && results) {
            uint32_t n_results = fi->type->n_results;
            for (uint32_t i = 0; i < n_results; i++) results[i] = ex->stack[ex->sp - n_results + i];
            ex->sp -= n_results;
        }
        ex->jb = saved;
        ret = rc;
    } else {
        ex->jb = saved;
        ex->sp = 0;
        if (trap) *trap = ex->trap;
        ret = 1;
    }
    ex->eh_top = eh_base; // trapped unwind skips generated handler pops
    g_fault_exec = saved_fault_exec;
    return ret;
}

int ea_store_load(EaStore *s, const uint8_t *bytes, size_t len, EaModule **out, char **err) {
    (void)s;
    EaModule *m = (EaModule *)ea_zalloc(sizeof(EaModule));
    int rc = ea_decode_module(m, bytes, len, err);
    if (rc != 0) {
        ea_module_free(m);
        free(m);
        return rc;
    }
    rc = ea_validate_module(m, err);
    if (rc != 0) {
        ea_module_free(m);
        free(m);
        return rc;
    }
    *out = m;
    return 0;
}
