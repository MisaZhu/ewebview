// easm utilities: allocator, names
#include "easm.h"
#include <stdio.h>
#include <stdlib.h>

void *ea_malloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) ea_fatal("oom");
    return p;
}
void *ea_realloc(void *p, size_t n) {
    void *r = realloc(p, n ? n : 1);
    if (!r) ea_fatal("oom");
    return r;
}
void *ea_zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) ea_fatal("oom");
    return p;
}
char *ea_strndup(const char *s, size_t n) {
    char *r = (char *)ea_malloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}
void ea_fatal(const char *msg) {
    fprintf(stderr, "easm fatal: %s\n", msg);
    abort();
}

const char *ea_vt_name(EaValType t) {
    switch (t) {
    case VT_I32: return "i32";
    case VT_I64: return "i64";
    case VT_F32: return "f32";
    case VT_F64: return "f64";
    case VT_V128: return "v128";
    case VT_FUNCREF: return "funcref";
    case VT_EXTERNREF: return "externref";
    default: return "?";
    }
}

const char *ea_trap_msg(EaTrap t) {
    switch (t) {
    case TRAP_UNREACHABLE: return "unreachable executed";
    case TRAP_DIV_BY_ZERO: return "integer divide by zero";
    case TRAP_INT_OVERFLOW: return "integer overflow";
    case TRAP_INVALID_CONV: return "invalid conversion to integer";
    case TRAP_OOB_MEMORY: return "out of bounds memory access";
    case TRAP_OOB_TABLE: return "out of bounds table access";
    case TRAP_UNDEF_ELEM: return "undefined element";
    case TRAP_UNINIT_ELEM: return "uninitialized element";
    case TRAP_INDIRECT_TYPE: return "indirect call type mismatch";
    case TRAP_NULL_DEREF: return "null reference";
    case TRAP_NULL_FUNC_REF: return "null function reference";
    case TRAP_STACK_EXHAUSTED: return "call stack exhausted";
    case TRAP_INDIRECT_CALL: return "indirect call";
    case TRAP_CAST: return "cast failure";
    default: return "trap";
    }
}
