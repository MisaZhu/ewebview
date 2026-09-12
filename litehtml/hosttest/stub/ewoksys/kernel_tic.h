#ifndef _STUB_EWOKSYS_KERNEL_TIC_H
#define _STUB_EWOKSYS_KERNEL_TIC_H
#include <stdint.h>
#include <sys/time.h>
static inline uint64_t kernel_tic_ms(uint64_t base)
{
    (void)base;
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)(tv.tv_usec / 1000);
}
#endif
