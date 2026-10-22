/* C99 runtime functions missing from the Xbox 360 SDK's C library. */
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xdk_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    int needed;
    va_list copy;

    /* C99 semantics: report the full length even when it doesn't fit. */
    copy = ap;
    needed = _vscprintf(fmt, copy);
    if (size > 0) {
        _vsnprintf(buf, size, fmt, ap);
        buf[size - 1] = '\0';
    }
    return needed;
}

int xdk_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = xdk_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

long xdk_lround(double x)
{
    return (long)(x < 0 ? ceil(x - 0.5) : floor(x + 0.5));
}

char *xdk_strndup(const char *s, size_t n)
{
    size_t len = 0;
    char *r;

    while (len < n && s[len])
        len++;
    r = (char *)malloc(len + 1);
    if (r) {
        memcpy(r, s, len);
        r[len] = '\0';
    }
    return r;
}

/*
 * XBMC-360's pthreads uses the debug monitor (xbdm) for asynchronous thread
 * cancellation, which FFmpeg never asks for. Linking xbdm.lib would stop the
 * .xex from loading on consoles that don't run xbdm, so these just fail.
 */
HRESULT DmGetThreadContext(DWORD thread_id, void *context)
{
    (void)thread_id;
    (void)context;
    return E_FAIL;
}

HRESULT DmSetThreadContext(DWORD thread_id, void *context)
{
    (void)thread_id;
    (void)context;
    return E_FAIL;
}
