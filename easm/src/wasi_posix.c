// POSIX backend (macOS / Linux)
#include "wasi_platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sched.h>
#if defined(__linux__)
#include <sys/random.h>
#endif

static int perr(void) {
    switch (errno) {
    case EACCES: return 2;      // acces
    case EBADF: return 8;
    case EEXIST: return 12;
    case EINVAL: return 21;
    case EIO: return 23;
    case EISDIR: return 28;
    case ENOENT: return 38;
    case ENOTDIR: return 54;
    case ENOMEM: return 42;
    default: return 23;         // io
    }
}

static int p_open(void *u, const char *path, int mode, int create, int trunc, int *pfd) {
    (void)u;
    int flags = O_CLOEXEC;
    if (mode == 0) flags |= O_RDONLY;
    else if (mode == 1) flags |= O_WRONLY;
    else flags |= O_RDWR;
    if (create) flags |= O_CREAT;
    if (trunc) flags |= O_TRUNC;
    int fd = open(path, flags, 0644);
    if (fd < 0) return perr();
    *pfd = fd;
    return 0;
}
static int p_close(void *u, int fd) { (void)u; return close(fd) == 0 ? 0 : perr(); }
static int p_read(void *u, int fd, uint8_t *buf, size_t len, size_t *nread) {
    (void)u;
    ssize_t r = read(fd, buf, len);
    if (r < 0) return perr();
    *nread = (size_t)r;
    return 0;
}
static int p_write(void *u, int fd, const uint8_t *buf, size_t len, size_t *nw) {
    (void)u;
    ssize_t r = write(fd, buf, len);
    if (r < 0) return perr();
    *nw = (size_t)r;
    return 0;
}
static int p_seek(void *u, int fd, int64_t off, int whence, uint64_t *newofs) {
    (void)u;
    off_t o = lseek(fd, (off_t)off, whence);
    if (o < 0) return perr();
    *newofs = (uint64_t)o;
    return 0;
}
static int p_sync(void *u, int fd) { (void)u; return fsync(fd) == 0 ? 0 : perr(); }
static int p_fsize(void *u, int fd, uint64_t *size) {
    (void)u;
    struct stat st;
    if (fstat(fd, &st) != 0) return perr();
    *size = (uint64_t)st.st_size;
    return 0;
}
static int p_isdir_path(void *u, const char *path, int *isdir) {
    (void)u;
    struct stat st;
    if (stat(path, &st) != 0) return perr();
    *isdir = S_ISDIR(st.st_mode);
    return 0;
}
static int p_mkdir(void *u, const char *path) { (void)u; return mkdir(path, 0755) == 0 ? 0 : perr(); }
static int p_unlink(void *u, const char *path) {
    (void)u;
    struct stat st;
    if (stat(path, &st) != 0) return perr();
    int r = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
    return r == 0 ? 0 : perr();
}
static int p_diropen(void *u, const char *path, int *ph) {
    (void)u;
    DIR *d = opendir(path);
    if (!d) return perr();
    *ph = (int)(intptr_t)d;
    return 0;
}
static int p_dirclose(void *u, int h) { (void)u; return closedir((DIR *)(intptr_t)h) == 0 ? 0 : perr(); }
static int p_dirnext(void *u, int h, char *name, size_t cap, int *isdir) {
    (void)u;
    struct dirent *de = readdir((DIR *)(intptr_t)h);
    if (!de) { name[0] = 0; return 0; }
    snprintf(name, cap, "%s", de->d_name);
    *isdir = (de->d_type == DT_DIR);
    return 0;
}
static int p_clock_ns(void *u, int id, uint64_t *out) {
    (void)u;
    struct timespec ts;
    clockid_t cid = id == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC;
    if (clock_gettime(cid, &ts) != 0) return 23;
    *out = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 0;
}
static int p_random(void *u, uint8_t *buf, size_t len) {
    (void)u;
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return 23;
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len ? 0 : 23;
}
static void p_yield(void *u) { (void)u; sched_yield(); }
static int p_sleep_ns(void *u, uint64_t ns) {
    (void)u;
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    nanosleep(&ts, NULL);
    return 0;
}

static const EaWasiPlat g_posix = {
    p_open, p_close, p_read, p_write, p_seek, p_sync, p_fsize,
    p_isdir_path, p_mkdir, p_unlink,
    p_diropen, p_dirclose, p_dirnext,
    p_clock_ns, p_random, p_yield, p_sleep_ns,
};
const EaWasiPlat *ea_wasi_plat_posix(void) { return &g_posix; }
