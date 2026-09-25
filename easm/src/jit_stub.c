// JIT stub: real aarch64 backend in jit_a64.c (phase 2)
#include "easm.h"
#include <stdio.h>

int ea_jit_call(EaExec *ex, EaFuncInst *fi) {
    (void)fi;
    ea_trap(ex, TRAP_INDIRECT_CALL);
    return 1;
}

void ea_jit_compile_module(EaModule *m) {
    (void)m;
}
