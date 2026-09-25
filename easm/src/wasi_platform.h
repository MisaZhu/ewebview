// platform porting layer for the WASI runtime
#ifndef EA_WASI_PLATFORM_H
#define EA_WASI_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

typedef struct EaWasiPlat EaWasiPlat;
struct EaWasiPlat {
    // regular files (path in UTF-8; mode: 0 ro, 1 wo, 2 rw)
    int (*open)(void *u, const char *path, int mode, int create, int trunc, int *pfd);
    int (*close)(void *u, int fd);
    int (*read)(void *u, int fd, uint8_t *buf, size_t len, size_t *nread);
    int (*write)(void *u, int fd, const uint8_t *buf, size_t len, size_t *nw);
    int (*seek)(void *u, int fd, int64_t off, int whence, uint64_t *newofs);
    int (*sync)(void *u, int fd);
    int (*fsize)(void *u, int fd, uint64_t *size);
    int (*isdir_path)(void *u, const char *path, int *isdir);
    int (*mkdir)(void *u, const char *path);
    int (*unlink)(void *u, const char *path);
    // directories (platform dir handles are separate from file fds)
    int (*diropen)(void *u, const char *path, int *ph);
    int (*dirclose)(void *u, int h);
    int (*dirnext)(void *u, int h, char *name, size_t cap, int *isdir); // "" = end
    // misc
    int (*clock_ns)(void *u, int id, uint64_t *out); // 0 realtime, 1 monotonic
    int (*random)(void *u, uint8_t *buf, size_t len);
    void (*yield)(void *u);
    int (*sleep_ns)(void *u, uint64_t ns);
};

// platform backends
#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
const EaWasiPlat *ea_wasi_plat_posix(void);
#endif
#ifdef _WIN32
const EaWasiPlat *ea_wasi_plat_win32(void);
#endif
// ewokos: HAL vtable supplied by the integrator + RAM-fs default
typedef struct EaEwokHal {
    int (*open)(void *u, const char *path, int mode, int create, int trunc, int *pfd);
    int (*close)(void *u, int fd);
    int (*read)(void *u, int fd, uint8_t *buf, size_t len, size_t *nread);
    int (*write)(void *u, int fd, const uint8_t *buf, size_t len, size_t *nw);
    int (*seek)(void *u, int fd, int64_t off, int whence, uint64_t *newofs);
    int (*fsize)(void *u, int fd, uint64_t *size);
    int (*unlink)(void *u, const char *path);
    int (*clock_ns)(void *u, int id, uint64_t *out);
    int (*random)(void *u, uint8_t *buf, size_t len);
} EaEwokHal;
const EaWasiPlat *ea_wasi_plat_ewok(const EaEwokHal *hal, void *user, void **out_user);
void *ea_ewok_ram_instance(void);
// RAM-fs HAL (portable C; used to verify the ewokos backend everywhere)
const EaEwokHal *ea_ewok_ramfs(void *u);

// posix is also the fallback so desktop builds always link
const EaWasiPlat *ea_wasi_plat_posix(void);
#endif
