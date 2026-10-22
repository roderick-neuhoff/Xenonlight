/*
 * Force-included into every C/C++ file of the Xbox 360 SDK build.
 * Fills the gaps between the SDK's Visual C++ 2010-era C runtime and the
 * C99 that moonlight-common-c and our own code expect.
 */
#ifndef XDK_COMPAT_H
#define XDK_COMPAT_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
/* Pull in the SDK's Win32/Winsock headers before any library wraps its own
 * includes in extern "C" (C++ headers inside extern "C" don't compile). */
#include <winsock2.h>

#ifndef __cplusplus
#  ifndef inline
#    define inline __inline
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* C99 snprintf/vsnprintf: always NUL-terminate, return the would-be length. */
int xdk_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int xdk_snprintf(char *buf, size_t size, const char *fmt, ...);
#define snprintf  xdk_snprintf
#define vsnprintf xdk_vsnprintf

/* Missing C99 math/string helpers. */
long xdk_lround(double x);
#define lround xdk_lround
char *xdk_strndup(const char *s, size_t n);
#define strdup _strdup
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s

#ifdef __cplusplus
}
#endif

/* C99 float min/max (fsel-friendly, no NaN handling needed here). */
static __inline float xdk_fmaxf(float a, float b) { return a > b ? a : b; }
static __inline float xdk_fminf(float a, float b) { return a < b ? a : b; }
#define fmaxf xdk_fmaxf
#define fminf xdk_fminf

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

#endif
