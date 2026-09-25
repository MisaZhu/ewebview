// ewokos backend: an embedded RTOS porting layer. The WASI platform
// adapter is portable C; the integrator supplies an EaEwokHal. A RAM
// filesystem HAL (ea_ewok_ramfs) ships as the default so the backend is
// verifiable on any host OS.
#include "wasi_platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------- RAM fs
#define EW_MAX_FILES 32
#define EW_MAX_SIZE (64 * 1024)

typedef struct {
    char *name;
    uint8_t *data;
    size_t size, cap, ofs;
    int used;
} EwFile;

typedef struct {
    EwFile files[EW_MAX_FILES];
    uint64_t clock;      // fake monotonic ns
    uint32_t rng;
} EwRam;

static EwFile *ew_find(EwRam *r, const char *path) {
    for (int i = 0; i < EW_MAX_FILES; i++)
        if (r->files[i].used && strcmp(r->files[i].name, path) == 0) return &r->files[i];
    return NULL;
}
static EwFile *ew_create(EwRam *r, const char *path) {
    for (int i = 0; i < EW_MAX_FILES; i++) {
        if (!r->files[i].used) {
            r->files[i].used = 1;
            r->files[i].name = strdup(path);
            r->files[i].data = NULL;
            r->files[i].size = r->files[i].cap = 0;
            return &r->files[i];
        }
    }
    return NULL;
}

static int ew_open(void *u, const char *path, int mode, int create, int trunc, int *pfd) {
    EwRam *r = (EwRam *)u;
    EwFile *f = ew_find(r, path);
    if (!f) {
        if (!create) return 38;
        f = ew_create(r, path);
        if (!f) return 45;
    } else if (trunc) { f->size = 0; f->data = NULL; f->cap = 0; }
    f->ofs = 0;
    (void)mode;
    *pfd = (int)(f - r->files);
    return 0;
}
static int ew_close(void *u, int fd) { (void)u; (void)fd; return 0; }
static int ew_read(void *u, int fd, uint8_t *buf, size_t len, size_t *nread) {
    EwRam *r = (EwRam *)u;
    EwFile *f = &r->files[fd];
    if (fd < 0 || fd >= EW_MAX_FILES || !f->used) return 8;
    size_t off = f->ofs > f->size ? f->size : f->ofs;
    size_t n = f->size - off < len ? f->size - off : len;
    if (n) memcpy(buf, f->data + off, n);
    f->ofs = off + n;
    *nread = n;
    return 0;
}
static int ew_write(void *u, int fd, const uint8_t *buf, size_t len, size_t *nw) {
    EwRam *r = (EwRam *)u;
    EwFile *f = &r->files[fd];
    if (fd < 0 || fd >= EW_MAX_FILES || !f->used) return 8;
    size_t off = f->ofs > f->size ? f->size : f->ofs;
    if (off + len > EW_MAX_SIZE) return 45;
    if (off + len > f->cap) {
        uint8_t *nd = (uint8_t *)realloc(f->data, off + len);
        if (!nd) return 42;
        f->data = nd;
        f->cap = off + len;
    }
    memcpy(f->data + off, buf, len);
    f->ofs = off + len;
    if (off + len > f->size) f->size = off + len;
    *nw = len;
    return 0;
}
static int ew_seek(void *u, int fd, int64_t off, int whence, uint64_t *newofs) {
    EwRam *r = (EwRam *)u;
    EwFile *f = &r->files[fd];
    int64_t base = whence == 0 ? 0 : (whence == 1 ? (int64_t)f->ofs : (int64_t)f->size);
    int64_t np = base + off;
    if (np < 0) return 21;
    f->ofs = (size_t)np;
    *newofs = (uint64_t)np;
    return 0;
}
static int ew_fsize(void *u, int fd, uint64_t *size) {
    EwRam *r = (EwRam *)u;
    if (fd < 0 || fd >= EW_MAX_FILES || !r->files[fd].used) return 8;
    *size = r->files[fd].size;
    return 0;
}
static int ew_unlink(void *u, const char *path) {
    EwRam *r = (EwRam *)u;
    EwFile *f = ew_find(r, path);
    if (!f) return 38;
    free(f->name); free(f->data);
    memset(f, 0, sizeof(*f));
    return 0;
}
static int ew_clock_ns(void *u, int id, uint64_t *out) {
    EwRam *r = (EwRam *)u;
    r->clock += 1000000; // 1 ms resolution fake clock
    *out = id == 0 ? r->clock + 1700000000000000000ull : r->clock;
    return 0;
}
static int ew_random(void *u, uint8_t *buf, size_t len) {
    EwRam *r = (EwRam *)u;
    for (size_t i = 0; i < len; i++) {
        r->rng = r->rng * 1103515245u + 12345u;
        buf[i] = (uint8_t)(r->rng >> 16);
    }
    return 0;
}

static const EaEwokHal g_ram_hal = {
    ew_open, ew_close, ew_read, ew_write, ew_seek, ew_fsize, ew_unlink,
    ew_clock_ns, ew_random,
};
static EwRam g_ram; // static storage: zero-init locals ✓, shared across calls
const EaEwokHal *ea_ewok_ramfs(void *u) {
    (void)u;
    return &g_ram_hal; // g_ram is the implicit user (set via ea_wasi_plat_ewok)
}
void *ea_ewok_ram_instance(void) { return &g_ram; }

// ------------------------------------------------------- platform adapter
// The ewok HAL has no dir surfaces (embedded filesystems are flat); those
// WASI calls return NOTSUP. Seek state is packed in the RAM-fs file cap.
typedef struct { const EaEwokHal *hal; void *user; } EwPlat;

static int ep_open(void *u, const char *path, int mode, int create, int trunc, int *pfd) {
    (void)mode;
    EwPlat *p = (EwPlat *)u;
    return p->hal->open(p->user, path, 2, create, trunc, pfd);
}
static int ep_close(void *u, int fd) { EwPlat *p = (EwPlat *)u; return p->hal->close(p->user, fd); }
static int ep_read(void *u, int fd, uint8_t *buf, size_t len, size_t *nread) {
    EwPlat *p = (EwPlat *)u;
    return p->hal->read(p->user, fd, buf, len, nread);
}
static int ep_write(void *u, int fd, const uint8_t *buf, size_t len, size_t *nw) {
    EwPlat *p = (EwPlat *)u;
    return p->hal->write(p->user, fd, buf, len, nw);
}
static int ep_seek(void *u, int fd, int64_t off, int whence, uint64_t *newofs) {
    EwPlat *p = (EwPlat *)u;
    return p->hal->seek(p->user, fd, off, whence, newofs);
}
static int ep_sync(void *u, int fd) { (void)u; (void)fd; return 0; }
static int ep_fsize(void *u, int fd, uint64_t *size) {
    EwPlat *p = (EwPlat *)u;
    return p->hal->fsize(p->user, fd, size);
}
static int ep_notsup_path(void *u, const char *path) { (void)u; (void)path; return 58; }
static int ep_notsup_int(void *u, int fd, uint64_t *s) { (void)u; (void)fd; (void)s; return 58; }
static int ep_notsup_isdir(void *u, const char *path, int *d) { (void)u; (void)path; (void)d; return 58; }
static int ep_diropen(void *u, const char *path, int *ph) { (void)u; (void)path; (void)ph; return 58; }
static int ep_dirclose(void *u, int h) { (void)u; (void)h; return 8; }
static int ep_dirnext(void *u, int h, char *name, size_t cap, int *isdir) {
    (void)u; (void)h; (void)name; (void)cap; (void)isdir;
    return 58;
}
static int ep_clock(void *u, int id, uint64_t *out) {
    EwPlat *p = (EwPlat *)u;
    return p->hal->clock_ns(p->user, id, out);
}
static int ep_random(void *u, uint8_t *buf, size_t len) {
    EwPlat *p = (EwPlat *)u;
    return p->hal->random(p->user, buf, len);
}
static void ep_yield(void *u) { (void)u; }
static int ep_sleep_ns(void *u, uint64_t ns) {
    EwPlat *p = (EwPlat *)u;
    uint64_t target;
    p->hal->clock_ns(p->user, 1, &target);
    target += ns;
    uint64_t now;
    do { p->hal->clock_ns(p->user, 1, &now); } while (now < target);
    return 0;
}

static int ep_mkdir_notsup(void *u, const char *path) { (void)u; (void)path; return 58; }
static int ep_unlink_via_hal(void *u, const char *path) {
    EwPlat *p = (EwPlat *)u;
    return p->hal->unlink(p->user, path);
}
static const EaWasiPlat g_ewok_plat = {
    ep_open, ep_close, ep_read, ep_write, ep_seek, ep_sync, ep_fsize,
    ep_notsup_isdir, ep_mkdir_notsup, ep_unlink_via_hal,
    ep_diropen, ep_dirclose, ep_dirnext,
    ep_clock, ep_random, ep_yield, ep_sleep_ns,
};
const EaWasiPlat *ea_wasi_plat_ewok(const EaEwokHal *hal, void *user, void **out_user) {
    static EwPlat p;
    p.hal = hal ? hal : ea_ewok_ramfs(NULL);
    p.user = user ? user : (void *)ea_ewok_ram_instance();
    *out_user = &p;
    return &g_ewok_plat;
}

