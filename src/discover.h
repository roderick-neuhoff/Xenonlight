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

/* Whether ip accepts a connection on port 47989 within timeout_ms. Unlike a
 * blocking connect, this gives up quickly on a sleeping PC. */
bool discover_probe(const char *ip, int timeout_ms);

/* Whether mac is "aa:bb:cc:dd:ee:ff" (and not all zeros, which hosts send
 * when they won't tell). */
bool wol_mac_valid(const char *mac);

/*
 * Sends Wake-on-LAN magic packets for mac: broadcast and to host_ip's /24
 * broadcast address, on the ports Moonlight's other clients use. Returns
 * false if nothing could be sent.
 */
bool wol_send(const char *mac, const char *host_ip);

#ifdef __cplusplus
}
#endif
