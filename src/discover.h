#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISCOVER_MAX 16

struct discover_progress {
    volatile int checked;       /* addresses finished so far */
    volatile int total;         /* addresses to check */
    volatile int found;
};

/*
 * Looks for GameStream hosts on the console's subnet (assumed /24): connects
 * to TCP port 47989 on every other address, a few at a time, and returns the
 * addresses that accept. console_ip is e.g. "192.168.1.99". Fills up to max
 * entries of out (dotted quads) and returns how many; -1 if console_ip isn't
 * usable. progress, if not NULL, is updated as the scan goes.
 */
int discover_hosts(const char *console_ip, char (*out)[16], int max, struct discover_progress *progress);

#ifdef __cplusplus
}
#endif
