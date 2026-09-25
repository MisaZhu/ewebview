// WASI Preview 1 (subset) over a platform porting layer.
#include "wasi.h"
#include "wasi_platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------- ctx
WasiCtx *ea_wasi_ctx_new(char **args, uint32_t n_args, char **env, uint32_t n_env) {
    WasiCtx *ctx = (WasiCtx *)calloc(1, sizeof(WasiCtx));
    ctx->args = args;
    ctx->n_args = n_args;
    ctx->env = env;
    ctx->n_env = n_env;
    ctx->next_fd = 0;
    return ctx;
}
void ea_wasi_ctx_free(WasiCtx *ctx) {
    if (!ctx) return;
    for (uint32_t i = 0; i < ctx->cap_fds; i++) {
        if (!ctx->fds[i].used) continue;
        free(ctx->fds[i].preopen_name);
        free(ctx->fds[i].base_path);
        free(ctx->fds[i].fb);
    }
    free(ctx->fds);
    free(ctx);
}

static WasiFd *fd_alloc(WasiCtx *ctx, uint32_t want) {
    if (want >= ctx->cap_fds) {
        uint32_t nc = ctx->cap_fds ? ctx->cap_fds * 2 : 16;
        if (nc <= want) nc = want + 1;
        ctx->fds = (WasiFd *)realloc(ctx->fds, nc * sizeof(WasiFd));
        memset(ctx->fds + ctx->cap_fds, 0, (nc - ctx->cap_fds) * sizeof(WasiFd));
        ctx->cap_fds = nc;
    }
    WasiFd *f = &ctx->fds[want];
    memset(f, 0, sizeof(*f));
    f->used = 1;
    if (want >= ctx->next_fd) ctx->next_fd = want + 1;
    return f;
}

static int fd_alloc_any(WasiCtx *ctx, uint32_t *pfd) {
    for (uint32_t i = 3; i < ctx->next_fd; i++)
        if (!ctx->fds[i].used) {
            memset(&ctx->fds[i], 0, sizeof(WasiFd));
            ctx->fds[i].used = 1;
            *pfd = i;
            return 0;
        }
    *pfd = ctx->next_fd < 3 ? 3 : ctx->next_fd;
    fd_alloc(ctx, *pfd);
    return 0;
}

int ea_wasi_preopen(WasiCtx *ctx, const char *name, const char *host_path) {
    uint32_t fd;
    fd_alloc_any(ctx, &fd);
    WasiFd *f = &ctx->fds[fd];
    f->kind = WASI_FT_DIR;
    f->preopen = 1;
    f->preopen_name = strdup(name);
    f->base_path = strdup(host_path);
    return fd;
}

int ea_wasi_framebuffer(WasiCtx *ctx, const char *name, size_t bytes) {
    uint32_t fd;
    fd_alloc_any(ctx, &fd);
    WasiFd *f = &ctx->fds[fd];
    f->kind = 4; // expose as regular file
    f->preopen = 1;
    f->preopen_name = strdup(name);
    f->base_path = strdup("efb:");
    f->fb = (uint8_t *)calloc(1, bytes);
    f->fb_cap = bytes;
    return fd;
}

int ea_wasi_fd_platfd(WasiCtx *ctx, int fd) {
    if (fd < 0 || (uint32_t)fd >= ctx->cap_fds || !ctx->fds[fd].used) return -1;
    return ctx->fds[fd].pf;
}

// ---------------------------------------------------------------- memory
static uint8_t *mem_ptr(WasiCtx *ctx, uint32_t off, uint32_t len) {
    if (!ctx->inst || ctx->inst->n_memories == 0) return NULL;
    EaMemInst *m = ctx->inst->memories[0];
    if ((uint64_t)off + len > m->size) return NULL;
    return m->base + off;
}
static uint32_t r32(WasiCtx *ctx, uint32_t off) {
    uint8_t *p = mem_ptr(ctx, off, 4);
    return p ? (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24) : 0;
}
static uint64_t r64(WasiCtx *ctx, uint32_t off) {
    uint8_t *p = mem_ptr(ctx, off, 8);
    if (!p) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void w32(WasiCtx *ctx, uint32_t off, uint32_t v) {
    uint8_t *p = mem_ptr(ctx, off, 4);
    if (!p) return;
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void w64(WasiCtx *ctx, uint32_t off, uint64_t v) {
    uint8_t *p = mem_ptr(ctx, off, 8);
    if (!p) return;
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// ---------------------------------------------------------------- syscalls
static const EaWasiPlat *plat_of(WasiCtx *ctx) { return ctx->plat; }

static int sys_args_sizes_get(WasiCtx *ctx, WVal *a, WVal *res) {
    uint32_t buflen = 0;
    for (uint32_t i = 0; i < ctx->n_args; i++) buflen += (uint32_t)strlen(ctx->args[i]) + 1;
    w32(ctx, a[0].i32, (uint32_t)ctx->n_args);
    w32(ctx, a[1].i32, buflen);
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_args_get(WasiCtx *ctx, WVal *a, WVal *res) {
    uint32_t ptr = a[0].i32, buf = a[1].i32;
    for (uint32_t i = 0; i < ctx->n_args; i++) {
        uint32_t len = (uint32_t)strlen(ctx->args[i]) + 1;
        w32(ctx, ptr, buf);
        uint8_t *b = mem_ptr(ctx, buf, len);
        if (!b) { res[0].i32 = 21; return 0; }
        memcpy(b, ctx->args[i], len);
        ptr += 4; buf += len;
    }
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_environ_sizes_get(WasiCtx *ctx, WVal *a, WVal *res) {
    uint32_t buflen = 0;
    for (uint32_t i = 0; i < ctx->n_env; i++) buflen += (uint32_t)strlen(ctx->env[i]) + 1;
    w32(ctx, a[0].i32, (uint32_t)ctx->n_env);
    w32(ctx, a[1].i32, buflen);
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_environ_get(WasiCtx *ctx, WVal *a, WVal *res) {
    uint32_t ptr = a[0].i32, buf = a[1].i32;
    for (uint32_t i = 0; i < ctx->n_env; i++) {
        uint32_t len = (uint32_t)strlen(ctx->env[i]) + 1;
        w32(ctx, ptr, buf);
        uint8_t *b = mem_ptr(ctx, buf, len);
        if (!b) { res[0].i32 = 21; return 0; }
        memcpy(b, ctx->env[i], len);
        ptr += 4; buf += len;
    }
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_clock_time_get(WasiCtx *ctx, WVal *a, WVal *res) {
    uint64_t t = 0;
    res[0].i32 = plat_of(ctx)->clock_ns(ctx->plat_user, a[0].i32, &t);
    w64(ctx, a[2].i32, t);
    return 0;
}
static int sys_clock_res_get(WasiCtx *ctx, WVal *a, WVal *res) {
    w32(ctx, a[1].i32, 1); // ns resolution
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_random_get(WasiCtx *ctx, WVal *a, WVal *res) {
    uint8_t *p = mem_ptr(ctx, a[0].i32, a[1].i32);
    if (!p) { res[0].i32 = 21; return 0; }
    res[0].i32 = plat_of(ctx)->random(ctx->plat_user, p, a[1].i32);
    return 0;
}

static WasiFd *fd_get(WasiCtx *ctx, uint32_t fd) {
    if (fd >= ctx->cap_fds || !ctx->fds[fd].used) return NULL;
    return &ctx->fds[fd];
}
// write the iovec array contents into a scratch list of (ptr, len)
typedef struct { uint32_t ptr, len; } Siov;
static Siov *iovs_get(WasiCtx *ctx, uint32_t ptr, uint32_t n, uint32_t *out_n) {
    if (n > 1024) return NULL;
    uint8_t *p = mem_ptr(ctx, ptr, n * 8);
    if (!p) return NULL;
    Siov *v = (Siov *)malloc(n * sizeof(Siov));
    for (uint32_t i = 0; i < n; i++) {
        v[i].ptr = r32(ctx, ptr + i * 8);
        v[i].len = r32(ctx, ptr + i * 8 + 4);
    }
    *out_n = n;
    return v;
}

static int sys_fd_write(WasiCtx *ctx, WVal *a, WVal *res) {
    uint32_t fd = a[0].i32, iovs = a[1].i32, n = a[2].i32;
    uint32_t nv = 0;
    Siov *v = iovs_get(ctx, iovs, n, &nv);
    if (!v) { res[0].i32 = 21; return 0; }
    WasiFd *f = fd_get(ctx, fd);
    if (getenv("EA_WDBG2")) fprintf(stderr, "[FW] fd=%u nv=%u f=%p fb=%p pf=%d\n", fd, nv, (void*)f, f ? (void*)f->fb : NULL, f ? f->pf : -1);
    if (!f) { free(v); res[0].i32 = 8; return 0; }
    int rc = 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < nv; i++) {
        uint8_t *p = mem_ptr(ctx, v[i].ptr, v[i].len);
        if (getenv("EA_WDBG2")) fprintf(stderr, "[FW] iov[%u] ptr=%u len=%u p=%p\n", i, v[i].ptr, v[i].len, (void*)p);
        if (!p) { rc = 21; break; }
        if (fd == 1 || fd == 2) { // std streams: host terminal, bypass platform
            size_t nw = fwrite(p, 1, v[i].len, fd == 1 ? stdout : stderr);
            rc = 0;
            total += (uint32_t)nw;
        } else if (f->fb) { // framebuffer device: append
            if (f->fb_len + v[i].len > f->fb_cap) { rc = 45; break; }
            memcpy(f->fb + f->fb_len, p, v[i].len);
            f->fb_len += v[i].len;
            total += v[i].len;
        } else {
            size_t nw = 0;
            rc = plat_of(ctx)->write(ctx->plat_user, f->pf, p, v[i].len, &nw);
            if (rc) break;
            total += (uint32_t)nw;
        }
    }
    free(v);
    w32(ctx, a[3].i32, total);
    res[0].i32 = rc;
    return 0;
}
static int sys_fd_read(WasiCtx *ctx, WVal *a, WVal *res) {
    uint32_t fd = a[0].i32, iovs = a[1].i32, n = a[2].i32;
    uint32_t nv = 0;
    Siov *v = iovs_get(ctx, iovs, n, &nv);
    if (!v) { res[0].i32 = 21; return 0; }
    WasiFd *f = fd_get(ctx, fd);
    if (getenv("EA_WDBG2")) fprintf(stderr, "[FR] fd=%u nv=%u f=%p used=%d cap=%u\n", fd, nv, (void*)f,
        (fd < ctx->cap_fds) ? (int)ctx->fds[fd].used : -9, ctx->cap_fds);
    if (!f) { free(v); res[0].i32 = 8; return 0; }
    int rc = 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < nv; i++) {
        uint8_t *p = mem_ptr(ctx, v[i].ptr, v[i].len);
        if (getenv("EA_WDBG2")) fprintf(stderr, "[FR] iov[%u] ptr=%u len=%u p=%p\n", i, v[i].ptr, v[i].len, (void*)p);
        if (!p) { rc = 21; break; }
        size_t nr = 0;
        rc = plat_of(ctx)->read(ctx->plat_user, f->pf, p, v[i].len, &nr);
        if (rc) break;
        total += (uint32_t)nr;
        if (nr < v[i].len) break; // short read
    }
    free(v);
    w32(ctx, a[3].i32, total);
    res[0].i32 = rc;
    return 0;
}
static int sys_fd_close(WasiCtx *ctx, WVal *a, WVal *res) {
    WasiFd *f = fd_get(ctx, a[0].i32);
    if (!f) { res[0].i32 = 8; return 0; }
    if (f->kind != WASI_FT_DIR && f->pf >= 0) res[0].i32 = plat_of(ctx)->close(ctx->plat_user, f->pf);
    else res[0].i32 = 0;
    if (f->kind == WASI_FT_DIR && f->pf >= 0) plat_of(ctx)->dirclose(ctx->plat_user, f->pf);
    free(f->preopen_name);
    free(f->base_path);
    memset(f, 0, sizeof(*f));
    return 0;
}
static int sys_fd_seek(WasiCtx *ctx, WVal *a, WVal *res) {
    WasiFd *f = fd_get(ctx, a[0].i32);
    if (!f) { res[0].i32 = 8; return 0; }
    uint64_t ofs = 0;
    res[0].i32 = plat_of(ctx)->seek(ctx->plat_user, f->pf, (int64_t)a[1].i64, a[2].i32, &ofs);
    w32(ctx, a[3].i32, (uint32_t)ofs);
    return 0;
}
static int sys_fd_tell(WasiCtx *ctx, WVal *a, WVal *res) {
    WasiFd *f = fd_get(ctx, a[0].i32);
    if (!f) { res[0].i32 = 8; return 0; }
    uint64_t ofs = 0;
    res[0].i32 = plat_of(ctx)->seek(ctx->plat_user, f->pf, 0, 1, &ofs);
    w32(ctx, a[1].i32, (uint32_t)ofs);
    return 0;
}
static int sys_fd_fdstat_get(WasiCtx *ctx, WVal *a, WVal *res) {
    WasiFd *f = fd_get(ctx, a[0].i32);
    if (!f) { res[0].i32 = 8; return 0; }
    uint8_t *p = mem_ptr(ctx, a[1].i32, 24);
    if (!p) { res[0].i32 = 21; return 0; }
    memset(p, 0, 24);
    p[0] = (uint8_t)f->kind;
    // rights: give the full set (minimal impl does not enforce)
    memset(p + 8, 0xff, 16);
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_fd_filestat_get(WasiCtx *ctx, WVal *a, WVal *res) {
    WasiFd *f = fd_get(ctx, a[0].i32);
    if (!f) { res[0].i32 = 8; return 0; }
    uint8_t *p = mem_ptr(ctx, a[1].i32, 64);
    if (!p) { res[0].i32 = 21; return 0; }
    memset(p, 0, 64);
    p[16] = (uint8_t)f->kind;
    uint64_t size = 0;
    if (f->kind == WASI_FT_REG) plat_of(ctx)->fsize(ctx->plat_user, f->pf, &size);
    else if (f->fb) size = f->fb_len;
    w64(ctx, a[1].i32 + 24, size);
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_fd_prestat_get(WasiCtx *ctx, WVal *a, WVal *res) {
    WasiFd *f = fd_get(ctx, a[0].i32);
    if (!f || !f->preopen || f->kind != WASI_FT_DIR) { res[0].i32 = 8; return 0; }
    uint8_t *p = mem_ptr(ctx, a[1].i32, 8);
    if (!p) { res[0].i32 = 21; return 0; }
    p[0] = 0; // preopendir
    w32(ctx, a[1].i32 + 4, (uint32_t)strlen(f->preopen_name));
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_fd_prestat_dir_name(WasiCtx *ctx, WVal *a, WVal *res) {
    WasiFd *f = fd_get(ctx, a[0].i32);
    if (!f || !f->preopen_name) { res[0].i32 = 8; return 0; }
    uint32_t cap = a[2].i32;
    uint32_t len = (uint32_t)strlen(f->preopen_name);
    if (len > cap) { res[0].i32 = 21; return 0; }
    uint8_t *p = mem_ptr(ctx, a[1].i32, cap ? cap : 1);
    if (!p) { res[0].i32 = 21; return 0; }
    memcpy(p, f->preopen_name, len);
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
// resolve "path" under the preopen dir of dirfd; rejects escapes
static char *resolve_path(WasiCtx *ctx, uint32_t dirfd, uint32_t path, uint32_t plen, int *err) {
    uint8_t *p = mem_ptr(ctx, path, plen ? plen : 1);
    if (!p) { *err = 21; return NULL; }
    WasiFd *f = fd_get(ctx, dirfd);
    if (!f || !f->base_path) { *err = 8; return NULL; }
    char rel[512];
    uint32_t n = plen < sizeof(rel) - 1 ? plen : (uint32_t)sizeof(rel) - 1;
    memcpy(rel, p, n);
    rel[n] = 0;
    if (strstr(rel, "..")) { *err = 2; return NULL; } // no escape
    char *full = (char *)malloc(strlen(f->base_path) + n + 2);
    int isfb = strcmp(f->base_path, "efb:") == 0;
    if (isfb) snprintf(full, strlen(f->base_path) + n + 2, "%s", rel);
    else snprintf(full, strlen(f->base_path) + n + 2, "%s/%s", f->base_path, rel);
    *err = 0;
    return full;
}
static int sys_path_open(WasiCtx *ctx, WVal *a, WVal *res) {
    uint32_t dirfd = a[0].i32;
    uint32_t path = a[2].i32, plen = a[3].i32;
    uint32_t oflags = a[4].i32;
    uint16_t fdflags = (uint16_t)(a[8].i32 & 0xffff);
    int err = 0;
    char *full = resolve_path(ctx, dirfd, path, plen, &err);
    if (err) { res[0].i32 = err; return 0; }
    // framebuffer preopen: path_open maps to the in-memory buffer
    WasiFd *dir = fd_get(ctx, dirfd);
    if (dir && dir->base_path && strcmp(dir->base_path, "efb:") == 0) {
        free(full);
        uint32_t nfd;
        fd_alloc_any(ctx, &nfd);
        WasiFd *f = &ctx->fds[nfd];
        f->kind = WASI_FT_REG;
        f->base_path = strdup("efb:");
        f->fb = dir->fb;       // share the device buffer
        f->fb_cap = dir->fb_cap;
        f->fb_len = (oflags & 0x10) ? 0 : dir->fb_len; // O_TRUNC clears
        dir->fb_len = f->fb_len;
        f->fb = dir->fb;
        res[0].i32 = WASI_E_SUCCESS;
        res[1].i32 = nfd;
        return 0;
    }
    int creat = (oflags & 0x01) != 0 || (oflags & 0x02) != 0; // CREAT | EXCL treated as create
    int trunc = (oflags & 0x08) != 0 || (fdflags & 0x01) != 0; // TRUNC | APPEND-std
    int isdir = 0;
    int rc = plat_of(ctx)->isdir_path(ctx->plat_user, full, &isdir);
    if (rc == 0 && isdir) {
        free(full);
        int ph = -1;
        rc = plat_of(ctx)->diropen(ctx->plat_user, full, &ph);
        free(full);
        if (rc) { res[0].i32 = rc; return 0; }
        uint32_t nfd;
        fd_alloc_any(ctx, &nfd);
        WasiFd *f = &ctx->fds[nfd];
        f->kind = WASI_FT_DIR;
        f->pf = ph;
        char *bp = (char *)malloc(1024);
        snprintf(bp, 1024, "%s", "");
        (void)bp;
        // dirfd opens under a preopen inherit the preopen base for nested paths
        w32(ctx, a[8].i32, nfd);
        res[0].i32 = WASI_E_SUCCESS;
        return 0;
    }
    int mode = 2; // rw
    int pf = -1;
    rc = plat_of(ctx)->open(ctx->plat_user, full, mode, creat, trunc, &pf);
    free(full);
    if (rc) { res[0].i32 = rc; return 0; }
    uint32_t nfd;
    fd_alloc_any(ctx, &nfd);
    WasiFd *f = &ctx->fds[nfd];
    f->kind = WASI_FT_REG;
    f->pf = pf;
    f->ofs = 0;
    w32(ctx, a[8].i32, nfd);
    if (getenv("EA_WDBG2")) fprintf(stderr, "[PO] opened nfd=%u pf=%d used=%d\n", nfd, f->pf, f->used);
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_fd_sync(WasiCtx *ctx, WVal *a, WVal *res) {
    WasiFd *f = fd_get(ctx, a[0].i32);
    if (!f) { res[0].i32 = 8; return 0; }
    res[0].i32 = f->kind == WASI_FT_REG ? plat_of(ctx)->sync(ctx->plat_user, f->pf) : 0;
    return 0;
}
static int sys_proc_exit(WasiCtx *ctx, WVal *a, WVal *res) {
    (void)res;
    ctx->exited = 1;
    ctx->exit_code = a[0].i32;
    return 1; // host trap -> invoke unwinds
}
static int sys_sched_yield(WasiCtx *ctx, WVal *r, WVal *res) {
    (void)r;
    plat_of(ctx)->yield(ctx->plat_user);
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}
static int sys_poll_oneoff(WasiCtx *ctx, WVal *a, WVal *res) {
    // clock subscriptions only: [u8 tag, u64 userdata, clock id, u64 timeout, u64 precision, u16 flags] * n
    uint32_t in = a[0].i32, out = a[1].i32, n = a[2].i32;
    if (n > 64) { res[0].i32 = 21; return 0; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sub = in + i * 48, ev = out + i * 32;
        uint8_t tag = 0;
        uint8_t *sp = mem_ptr(ctx, sub, 48);
        if (!sp) { res[0].i32 = 21; return 0; }
        tag = sp[0];
        uint64_t userdata = r64(ctx, sub + 8);
        uint64_t timeout = r64(ctx, sub + 24);
        uint16_t flags = (uint16_t)r32(ctx, sub + 40);
        w64(ctx, ev, userdata);
        uint16_t errv = 0, type = 0;
        if (tag == 0) { errv = 58; } // clock subscriptions only
        else if (flags & 1) { // relative: sleep
            plat_of(ctx)->sleep_ns(ctx->plat_user, timeout);
            errv = 0;
        } else { errv = 0; }
        w32(ctx, ev + 16, (uint32_t)(errv | ((uint32_t)type << 16)));
        w32(ctx, ev + 20, 0);
    }
    w32(ctx, a[3].i32, n);
    res[0].i32 = WASI_E_SUCCESS;
    return 0;
}

// ---------------------------------------------------------------- dispatch
typedef struct { WasiCtx *ctx; int id; } WasiThunk;
static int wasi_dispatch(void *user, const WVal *args, WVal *results) {
    WasiThunk *t = (WasiThunk *)user;
    WasiCtx *ctx = t->ctx;
    WVal *a = (WVal *)args;
    switch (t->id) {
    case 0: return sys_args_sizes_get(ctx, a, results);
    case 1: return sys_args_get(ctx, a, results);
    case 2: return sys_environ_sizes_get(ctx, a, results);
    case 3: return sys_environ_get(ctx, a, results);
    case 4: return sys_clock_time_get(ctx, a, results);
    case 5: return sys_clock_res_get(ctx, a, results);
    case 6: return sys_random_get(ctx, a, results);
    case 7: return sys_fd_write(ctx, a, results);
    case 8: return sys_fd_read(ctx, a, results);
    case 9: return sys_fd_close(ctx, a, results);
    case 10: return sys_fd_seek(ctx, a, results);
    case 11: return sys_fd_tell(ctx, a, results);
    case 12: return sys_fd_fdstat_get(ctx, a, results);
    case 13: return sys_fd_filestat_get(ctx, a, results);
    case 14: return sys_fd_prestat_get(ctx, a, results);
    case 15: return sys_fd_prestat_dir_name(ctx, a, results);
    case 16: return sys_path_open(ctx, a, results);
    case 17: return sys_fd_sync(ctx, a, results);
    case 18: return sys_proc_exit(ctx, a, results);
    case 19: return sys_sched_yield(ctx, a, results);
    default: return sys_poll_oneoff(ctx, a, results);
    }
}

// ---------------------------------------------------------------- instance
static EaValType T_I32 = VT_I32, T_I64 = VT_I64;

typedef struct { const char *name; int id; uint32_t np, nr; } WasiSig;
// preview1 convention: errno-only results, out-values via pointer params
static const WasiSig g_sigs[] = {
    {"args_sizes_get", 0, 2, 1},
    {"args_get", 1, 2, 1},
    {"environ_sizes_get", 2, 2, 1},
    {"environ_get", 3, 2, 1},
    {"clock_time_get", 4, 3, 1},
    {"clock_res_get", 5, 2, 1},
    {"random_get", 6, 2, 1},
    {"fd_write", 7, 4, 1},
    {"fd_read", 8, 4, 1},
    {"fd_close", 9, 1, 1},
    {"fd_seek", 10, 4, 1},
    {"fd_tell", 11, 2, 1},
    {"fd_fdstat_get", 12, 2, 1},
    {"fd_filestat_get", 13, 2, 1},
    {"fd_prestat_get", 14, 2, 1},
    {"fd_prestat_dir_name", 15, 3, 1},
    {"path_open", 16, 9, 1},
    {"fd_sync", 17, 1, 1},
    {"proc_exit", 18, 1, 0},
    {"sched_yield", 19, 0, 1},
    {"poll_oneoff", 20, 4, 1},
};

int ea_wasi_init(EaStore *store, WasiCtx *ctx, char **err) {
    (void)err;
    // platform selection: default posix, EA_BACKEND=ewok opts into the HAL layer
    const char *backend = getenv("EA_BACKEND");
    if (backend && strcmp(backend, "ewok") == 0)
        ctx->plat = ea_wasi_plat_ewok(NULL, NULL, &ctx->plat_user);
    else {
        ctx->plat = ea_wasi_plat_posix();
        ctx->plat_user = ctx;
    }

    // std fds
    WasiFd *f0 = fd_alloc(ctx, 0); f0->kind = WASI_FT_UNKNOWN; f0->pf = 0;
    WasiFd *f1 = fd_alloc(ctx, 1); f1->kind = WASI_FT_UNKNOWN; f1->pf = 1;
    WasiFd *f2 = fd_alloc(ctx, 2); f2->kind = WASI_FT_UNKNOWN; f2->pf = 2;

    uint32_t n = (uint32_t)(sizeof(g_sigs) / sizeof(g_sigs[0]));
    EaModule *m = (EaModule *)calloc(1, sizeof(EaModule));
    m->n_types = n;
    m->types = (EaType *)calloc(n, sizeof(EaType));
    m->n_funcs = n;
    m->funcs = (EaFunc *)calloc(n, sizeof(EaFunc));
    m->n_exports = n;
    m->exports = (EaExport *)calloc(n, sizeof(EaExport));
    EaInstance *inst = (EaInstance *)calloc(1, sizeof(EaInstance));
    inst->n_funcs = n;
    inst->funcs = (EaFuncInst *)calloc(n, sizeof(EaFuncInst));
    WasiThunk **thunks = (WasiThunk **)calloc(n, sizeof(WasiThunk *));

    for (uint32_t i = 0; i < n; i++) {
        const WasiSig *sig = &g_sigs[i];
        EaType *t = &m->types[i];
        t->kind = CT_FUNC;
        t->is_final = 1;
        t->rec_pos = 0;  // singleton rec group: canonical-eq compares structurally
        t->rec_size = 1;
        t->func.n_params = sig->np;
        t->func.n_results = sig->nr;
        // build param/result arrays: real storage per signature
        EaValType *pv = (EaValType *)calloc(sig->np ? sig->np : 1, sizeof(EaValType));
        EaValType *rv = (EaValType *)calloc(sig->nr ? sig->nr : 1, sizeof(EaValType));
        for (uint32_t k = 0; k < sig->np; k++) pv[k] = T_I32;
        // clock_time_get: (i32, i64) -> (i32, i64); fd_seek: (i32,i64,i32)->(i32,i64)
        if (sig->id == 4 || sig->id == 10) pv[1] = T_I64;
        if (sig->id == 16) { pv[5] = T_I64; pv[6] = T_I64; }
        for (uint32_t k = 0; k < sig->nr; k++) rv[k] = T_I32;
        if (sig->id == 4 || sig->id == 5 || sig->id == 10) rv[1] = T_I64;
        t->func.params = pv;
        t->func.results = rv;

        m->funcs[i].type_idx = i;
        m->exports[i].name = strdup(sig->name);
        m->exports[i].name_len = (uint32_t)strlen(sig->name);
        m->exports[i].kind = EAK_FUNC;
        m->exports[i].idx = i;

        inst->funcs[i].type = &t->func;
        inst->funcs[i].type_idx = i;
        inst->funcs[i].inst = inst;
        inst->funcs[i].is_host = true;
        thunks[i] = (WasiThunk *)malloc(sizeof(WasiThunk));
        thunks[i]->ctx = ctx;
        thunks[i]->id = sig->id;
        inst->funcs[i].host_fn = wasi_dispatch;
        inst->funcs[i].host_user = thunks[i];
    }
    (void)T_I64;
    inst->module = m;
    ctx->inst = inst; // host instance doubles as the memory source until bind
    ea_register_instance(store, "wasi_snapshot_preview1", inst);
    return 0;
}
