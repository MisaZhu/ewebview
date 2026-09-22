// WASI Preview 1 (subset) over a platform porting layer.
// platforms: posix (macOS/Linux), win32, ewokos (HAL vtable + RAM-fs)
#ifndef EA_WASI_H
#define EA_WASI_H

#include "easm.h"
#include <stdint.h>
#include <stddef.h>

// WASI errno (subset we return)
#define WASI_E_SUCCESS 0
#define WASI_E_ACCES 2
#define WASI_E_BADF 8
#define WASI_E_EXIST 12
#define WASI_E_INVAL 21
#define WASI_E_IO 23
#define WASI_E_ISDIR 28
#define WASI_E_NOENT 38
#define WASI_E_NOTDIR 54
#define WASI_E_NOMEM 42
#define WASI_E_NOTSUP 58
#define WASI_E_PERM 71

// WASI filetypes
#define WASI_FT_UNKNOWN 0
#define WASI_FT_DIR 3
#define WASI_FT_REG 4

typedef struct EaWasiPlat EaWasiPlat;

// per-fd state
typedef struct {
    int used;
    int kind;            // WASI_FT_*
    int pf;              // platform fd/handle
    uint64_t ofs;
    char *preopen_name;  // set for preopen dirs
    char *base_path;     // host path backing a dir preopen
    int preopen;
    uint8_t *fb;         // framebuffer backing (guest writes)
    size_t fb_len, fb_cap;
} WasiFd;

typedef struct WasiCtx {
    EaInstance *inst;          // bound after instantiation
    EaWasiPlat *plat;
    void *plat_user;
    // args/env (host-owned strings)
    char **args;
    uint32_t n_args;
    char **env;
    uint32_t n_env;
    // fd table
    WasiFd *fds;
    uint32_t n_fds, cap_fds;
    uint32_t next_fd;
    // state
    int exited;
    uint32_t exit_code;
} WasiCtx;

// build the "wasi_snapshot_preview1" host instance and register it
int ea_wasi_init(EaStore *store, WasiCtx *ctx, char **err);
// bind the guest instance after instantiation (memory access)
static inline void ea_wasi_bind(WasiCtx *ctx, EaInstance *inst) { ctx->inst = inst; }

// CLI context assembly
WasiCtx *ea_wasi_ctx_new(char **args, uint32_t n_args, char **env, uint32_t n_env);
void ea_wasi_ctx_free(WasiCtx *ctx);
int ea_wasi_preopen(WasiCtx *ctx, const char *name, const char *host_path); // dir preopen
int ea_wasi_framebuffer(WasiCtx *ctx, const char *name, size_t bytes);      // efb device
int ea_wasi_fd_platfd(WasiCtx *ctx, int fd); // platform fd (for the viewer)

#endif
