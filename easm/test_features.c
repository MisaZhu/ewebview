#include "easm.h"
#include <stdio.h>
#include <stdint.h>

static int probe(const char* name, const uint8_t *b, uint32_t n) {
    EaStore* s = ea_store_new();
    ea_store_set_max_depth(s, 1000);
    char* err = NULL;
    EaModule* m = NULL;
    int rc = ea_store_load(s, b, n, &m, &err);
    if (rc == 0) { ea_module_free(m); ea_store_free(s); printf("%-28s PASS (decodes)\n", name); return 0; }
    printf("%-28s FAIL: %s\n", name, err ? err : "?");
    if (err) free(err);
    ea_store_free(s);
    return 1;
}

int main(void) {
    /* exact probes from wasm.joway.io inline script */
    static const uint8_t SIMD[] = {0,97,115,109,1,0,0,0, 1,5,1,96,0,1,123, 3,2,1,0, 10,10,1,8,0, 65,0,253,15,253,98,11};
    static const uint8_t THREADS[] = {0,97,115,109,1,0,0,0, 1,4,1,96,0,0, 3,2,1,0, 5,4,1,3,1,1, 10,11,1,9,0, 65,0,254,16,2,0,26,11};
    static const uint8_t BULK[] = {0,97,115,109,1,0,0,0, 1,4,1,96,0,0, 3,2,1,0, 5,3,1,0,1, 10,14,1,12,0, 65,0,65,0,65,0,252,10,0,0,11};
    static const uint8_t REFF[] = {0,97,115,109,1,0,0,0, 1,4,1,96,0,0, 3,2,1,0, 10,7,1,5,0, 208,112,26,11};
    static const uint8_t EH[] = {0,97,115,109,1,0,0,0, 1,4,1,96,0,0, 3,2,1,0, 10,8,1,6,0, 6,64,25,11,11};
    static const uint8_t TAIL[] = {0,97,115,109,1,0,0,0, 1,4,1,96,0,0, 3,2,1,0, 10,6,1,4,0, 18,0,11};
    static const uint8_t MULTIVAL[] = {0,97,115,109,1,0,0,0, 1,6,1,96,0,2,127,127, 3,2,1,0, 10,8,1,6,0, 65,0,65,0,11};

    int fails = 0;
    fails += probe("SIMD", SIMD, sizeof(SIMD));
    fails += probe("Threads (shared mem)", THREADS, sizeof(THREADS));
    fails += probe("Bulk memory ops", BULK, sizeof(BULK));
    fails += probe("Reference types", REFF, sizeof(REFF));
    fails += probe("Exception handling", EH, sizeof(EH));
    fails += probe("Tail calls", TAIL, sizeof(TAIL));
    fails += probe("Multi-value returns", MULTIVAL, sizeof(MULTIVAL));
    (void)fails;
    return 0;
}