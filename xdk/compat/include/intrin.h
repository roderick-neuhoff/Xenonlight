/*
 * x86 <intrin.h> subset for code that assumes MSVC means x86 (Opus, nanors).
 * Implemented with the Xbox 360 compiler's PowerPC intrinsics.
 */
#ifndef XDK_INTRIN_H
#define XDK_INTRIN_H

#include <ppcintrinsics.h>

/* "#pragma intrinsic(_BitScanReverse)" would warn: these are inline functions. */
#pragma warning(disable: 4163)

static __inline unsigned char _BitScanReverse(unsigned long *index, unsigned long mask)
{
    if (mask == 0)
        return 0;
    *index = 31 - _CountLeadingZeros((long)mask);
    return 1;
}

static __inline unsigned char _BitScanForward(unsigned long *index, unsigned long mask)
{
    if (mask == 0)
        return 0;
    /* Isolate the lowest set bit, then count from the top. */
    *index = 31 - _CountLeadingZeros((long)(mask & (0UL - mask)));
    return 1;
}

#endif
