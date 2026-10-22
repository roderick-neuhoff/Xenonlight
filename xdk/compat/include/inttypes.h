/* Minimal C99 <inttypes.h> for the Xbox 360 SDK's C runtime. */
#ifndef XDK_INTTYPES_H
#define XDK_INTTYPES_H
#include <stdint.h>
#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "I64d"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "I64u"
#define PRIx32 "x"
#define PRIx64 "I64x"
#define PRIX32 "X"
#define PRIX64 "I64X"
/* Pointers and size_t are 32-bit on the Xbox 360. */
#define PRIuPTR "u"
#define PRIdPTR "d"
#define PRIxPTR "x"
#endif
