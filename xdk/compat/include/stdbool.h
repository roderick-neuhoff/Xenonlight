/* C99 <stdbool.h> for the Xbox 360 SDK's C compiler (C++ has bool built in). */
#ifndef XDK_STDBOOL_H
#define XDK_STDBOOL_H
#ifndef __cplusplus
typedef unsigned char _xdk_bool;
#define bool  _xdk_bool
#define true  1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif
