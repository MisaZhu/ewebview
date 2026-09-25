#include "easm.h"
#include <stdio.h>
#include <stdint.h>

/* (module (func (export "add") (param i32 i32) (result i32)
 *          local.get 0 local.get 1 i32.add)) */
static const uint8_t wasm_add[] = {
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
    0x03,0x02,0x01,0x00,
    0x07,0x07,0x01,0x03,0x61,0x64,0x64,0x00,0x00,
    0x0a,0x09,0x01,0x07,0x00,0x20,0x00,0x20,0x01,0x6a,0x0b,
};

/* memory grow test: (module (memory 1 2) (export "grow" (func $g))
 *  $g (func (param i32) (result i32) (memory.grow (local.get 0))))
 * grow(1) should return 1 (old pages). major must be 2 after.
 */
int main(void) {
    EaStore* s = ea_store_new();
    ea_store_set_max_depth(s, 100000);
    char* err = NULL;
    EaModule* m = NULL;

    if (ea_store_load(s, wasm_add, sizeof(wasm_add), &m, &err) != 0) {
        fprintf(stderr, "load failed: %s\n", err ? err : "?");
        return 1;
    }
    ea_jit_compile_module(m);
    EaInstance* inst = NULL;
    EaTrap trap = TRAP_NONE;
    if (ea_store_instantiate(s, m, &inst, &err, &trap) != 0) {
        fprintf(stderr, "instantiate failed: %s trap=%d\n", err ? err : "?", (int)trap);
        return 1;
    }
    int ex = ea_instance_export(inst, "add", EAK_FUNC);
    if (ex < 0) { fprintf(stderr, "no add export\n"); return 1; }
    WVal args[2] = {{.i32=30},{.i32=12}}, res[1];
    if (ea_instance_invoke(s, inst, (uint32_t)ex, args, res, &trap) != 0) {
        fprintf(stderr, "invoke failed trap=%d\n", (int)trap);
        return 1;
    }
    /* machine is little-endian arm64; union access to i32 is what easm uses */
    int32_t got = (int32_t)(uint32_t)res[0].i32;
    printf("add(30,12) = %d (expect 42)\n", got);
    ea_instance_free(inst);
    ea_module_free(m);
    ea_store_free(s);
    return got == 42 ? 0 : 2;
}