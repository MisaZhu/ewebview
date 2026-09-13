/*
 * compat.c - desktop stubs for EwokOS-specific symbols referenced by the
 * ewebview / litehtml / tinyhttpsc / gumbo libraries.
 *
 * The embedded libraries were written for EwokOS and reference a handful of
 * platform primitives directly (not through the HAL).  On a desktop OS those
 * symbols do not exist, so we provide minimal portable replacements here.
 *
 * Plain C99.
 */

#include <setjmp.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* gumbo parser OOM jump buffer                                        */
/* ------------------------------------------------------------------ */

/* gumbo/parser.c and gumbo/util.c declare these as extern and call them
 * instead of raw setjmp/longjmp so the OOM path can be redirected.  On
 * EwokOS the libc provides them; on desktop we just forward to the real
 * setjmp/longjmp.  The jmp_buf itself is defined in parser.c. */

extern jmp_buf gumbo_oom_jmpbuf;

int gumbo_setjmp(jmp_buf env) {
    memcpy(gumbo_oom_jmpbuf, env, sizeof(jmp_buf));
    return setjmp(gumbo_oom_jmpbuf);
}

void gumbo_longjmp(jmp_buf env, int val) {
    memcpy(gumbo_oom_jmpbuf, env, sizeof(jmp_buf));
    longjmp(gumbo_oom_jmpbuf, val);
}

/* ------------------------------------------------------------------ */
/* litehtml heap-pointer plausibility test                             */
/* ------------------------------------------------------------------ */

/* html_tag.cpp calls ewok_ptr_in_heap() to reject forged element handles
 * before reading a liveness tag through them.  On desktop OSes there is no
 * cheap non-dereferencing heap-membership test, so we return true (trust the
 * liveness tag alone - the same degradation the HAL documents when
 * sys.ptr_sane is NULL). */

int ewok_ptr_in_heap(const void* p) {
    return (p != NULL) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* tinyhttpsc entropy sleep                                            */
/* ------------------------------------------------------------------ */

/* BearHttpsClientOne.c calls proc_usleep(1) as a timing-entropy source for
 * the BearSSL RNG seed.  On desktop we use nanosleep. */

void proc_usleep(uint32_t us) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)((us % 1000000u) * 1000u);
    nanosleep(&ts, NULL);
}
