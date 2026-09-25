// easm aarch64 JIT — internal layer contract (jit_a64.c / jit_a64_rt.c).
//
// The backend is split in two layers:
//
//   jit_a64.c      codegen: JC compiler state, stack primitives, the fusion
//                  window, all emitters and the compile driver.  Kept as a
//                  single translation unit on purpose — the stack
//                  primitives are one-instruction wrappers that must inline.
//   jit_a64_rt.c   runtime: everything generated code calls *into* (traps,
//                  bulk-memory/table helpers, float helpers, exception
//                  unwinding, the interpreter bridge) plus the C->JIT
//                  trampolines and the 16MB code region.  No compiler
//                  state.
//
// Register conventions assumed by generated code:
//   x25 = memory base, x26 = memory limit (bytes),
//   x27 = EaExec*,      x28 = EaInstance*,
//   x16/x17 = scratch,  x19-x24 = local cache file (callee-saved).
#ifndef EA_JIT_A64_H
#define EA_JIT_A64_H

#include "easm.h"

// code region (jit_a64_rt.c)
void jit_wren(int on);            // toggle W^X for this thread
uint8_t *region_alloc(size_t bytes); // bump-allocate from the 16MB JIT region

// raised from generated code on explicit bounds/trap checks
void ea_jit_trap_now(EaExec *ex, uint32_t code);

// interpreter bridge: run fi in the interpreter, results on the valstack
uint64_t ea_jit_call_interp(EaExec *ex, EaFuncInst *fi, WVal *args);

// exception unwinding (try_table support)
EaEhRet ea_jit_eh_resume(EaExec *ex, EaInstance *inst, void *fp);
EaEhRet ea_jit_eh_throw(EaExec *ex, EaInstance *inst, uint32_t tag_idx, WVal *sp, void *fp);
EaEhRet ea_jit_eh_throw_ref(EaExec *ex, EaInstance *inst, EaExnInst *exn, void *fp);
void ea_jit_eh_push(EaExec *ex, EaInstance *inst, EaEhDesc *desc, WVal *sp0, void *fp);
void ea_jit_eh_pop(EaExec *ex);
void ea_jit_eh_popn(EaExec *ex, uint32_t n);

// call_indirect target resolution + signature check (returns NULL-fi on trap)
EaFuncInst *ea_jit_callee_lookup(EaExec *ex, EaInstance *inst, uint32_t table_idx,
                                 uint32_t type_idx, uint32_t elem_idx);

// debug dumps, emitted under EA_WDBG2 (see jit_a64.c)
void ea_h_spdump(uint64_t tag, uint64_t sp);
void ea_h_wdump6(uint64_t fp, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);

// float->int truncation with fused wasm trap semantics
int32_t ea_h_trunc_i32_f32(EaExec *ex, uint32_t bits);
int32_t ea_h_trunc_i32_f64(EaExec *ex, double f);
uint32_t ea_h_trunc_u32_f32(EaExec *ex, uint32_t bits);
uint32_t ea_h_trunc_u32_f64(EaExec *ex, double f);
int64_t ea_h_trunc_i64_f32(EaExec *ex, uint32_t bits);
int64_t ea_h_trunc_i64_f64(EaExec *ex, double f);
uint64_t ea_h_trunc_u64_f32(EaExec *ex, uint32_t bits);
uint64_t ea_h_trunc_u64_f64(EaExec *ex, double f);

// trace probes, emitted under EA_JIT_TRACE
void ea_h_trace(uint64_t marker);
void ea_h_trace2(uint64_t marker, uint64_t sp);
void ea_h_trace3(uint64_t marker, uint64_t slot);

// float->int truncation with fused wasm trap semantics
int32_t ea_h_trunc_i32_f32(EaExec *ex, uint32_t bits);
int32_t ea_h_trunc_i32_f64(EaExec *ex, double f);
uint32_t ea_h_trunc_u32_f32(EaExec *ex, uint32_t bits);
uint32_t ea_h_trunc_u32_f64(EaExec *ex, double f);
int64_t ea_h_trunc_i64_f32(EaExec *ex, uint32_t bits);
int64_t ea_h_trunc_i64_f64(EaExec *ex, double f);
uint64_t ea_h_trunc_u64_f32(EaExec *ex, uint32_t bits);
uint64_t ea_h_trunc_u64_f64(EaExec *ex, double f);

// trace probes, emitted under EA_JIT_TRACE
void ea_h_trace(uint64_t marker);
void ea_h_trace2(uint64_t marker, uint64_t sp);
void ea_h_trace3(uint64_t marker, uint64_t slot);

// f32/f64 minmax with wasm NaN/zero semantics (used by fmin/fmax emitters)

// bulk memory / table / memory-grow helpers (call_helper targets)

// bulk memory / table / memory-grow helpers (call_helper targets)
uint64_t ea_h_popcnt64(uint64_t x);
void ea_h_spdump(uint64_t tag, uint64_t sp);
void ea_h_wdump6(uint64_t fp, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f);
uint32_t ea_h_f32_min(uint32_t ab, uint32_t bb);
uint32_t ea_h_f32_max(uint32_t ab, uint32_t bb);
uint64_t ea_h_f64_min(uint64_t ab, uint64_t bb);
uint64_t ea_h_f64_max(uint64_t ab, uint64_t bb);
int32_t ea_h_trunc_i32_f32(EaExec *ex, uint32_t bits);
int32_t ea_h_trunc_i32_f64(EaExec *ex, double f);
uint32_t ea_h_trunc_u32_f32(EaExec *ex, uint32_t bits);
uint32_t ea_h_trunc_u32_f64(EaExec *ex, double f);
int64_t ea_h_trunc_i64_f32(EaExec *ex, uint32_t bits);
int64_t ea_h_trunc_i64_f64(EaExec *ex, double f);
uint64_t ea_h_trunc_u64_f32(EaExec *ex, uint32_t bits);
uint64_t ea_h_trunc_u64_f64(EaExec *ex, double f);
void ea_h_trace(uint64_t marker);
void ea_h_trace3(uint64_t marker, uint64_t slot);

WVal *ea_h_memory_size(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx);
WVal *ea_h_memory_grow(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx);
WVal *ea_h_memory_fill(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx);
WVal *ea_h_memory_copy(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dst, uint32_t src);
WVal *ea_h_memory_init(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t memidx, uint32_t dataidx);
WVal *ea_h_data_drop(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dataidx, uint32_t unused);
WVal *ea_h_table_get(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx);
WVal *ea_h_table_set(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx);
WVal *ea_h_table_size(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx);
WVal *ea_h_table_grow(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx);
WVal *ea_h_table_fill(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx);
WVal *ea_h_table_copy(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t dtn, uint32_t stn);
WVal *ea_h_table_init(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t tidx, uint32_t eidx);
WVal *ea_h_elem_drop(EaExec *ex, EaInstance *inst, WVal *sp, uint32_t eidx, uint32_t unused);

#endif
