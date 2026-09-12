#ifndef _STUB_EWOKSYS_KLOG_H
#define _STUB_EWOKSYS_KLOG_H
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
static inline void klog(const char* fmt, ...)
{
    if (!getenv("LH_DEBUG")) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
#endif
