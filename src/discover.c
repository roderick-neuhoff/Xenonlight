/*
 * Finds GameStream hosts by trying TCP port 47989 (the host's HTTP port) on
 * every address of the console's /24. Neither build has multicast, so mDNS
 * isn't an option; a connect scan works on both network stacks.
 *
 * Connects are non-blocking and kept a fixed number in flight. A host that
 * exists answers within a few milliseconds (accept or reset); an empty
 * address never answers ARP, so it costs one CONNECT_TIMEOUT_MS.
 */
#include "discover.h"

#include <stdio.h>
#include <string.h>

#include "platform.h"

#ifdef _XBOX
#  include <winsock2.h>
#  define sock_close closesocket
#  define sock_in_progress() (WSAGetLastError() == WSAEWOULDBLOCK)
/* XNet has sockets to spare before streaming starts. */
#  define WINDOW 24
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  define sock_close close
#  define sock_in_progress() (errno == EINPROGRESS)
/* lwIP's ARP table has 10 entries; more pending lookups than that start
 * evicting each other and real hosts get missed. */
#  define WINDOW 8
#endif

#define HTTP_PORT 47989
#define CONNECT_TIMEOUT_MS 400

struct probe {
    int fd;                     /* -1 when the slot is free */
    unsigned int host;          /* last octet */
    uint32_t started;
};

static int set_nonblocking(int fd)
{
#ifdef _XBOX
    u_long one = 1;
    return ioctlsocket(fd, FIONBIO, &one);
#else
    return fcntl(fd, F_SETFL, O_NONBLOCK);
#endif
}

/* Starts a connect to prefix.host; returns the socket, or -1 (address skipped). */
static int probe_start(uint32_t prefix, unsigned int host)
{
    struct sockaddr_in sa;
    int fd;

    fd = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    if (set_nonblocking(fd) != 0) {
        sock_close(fd);
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(HTTP_PORT);
    sa.sin_addr.s_addr = htonl(prefix | host);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
        return fd;   /* finished at once; select() reports it writable */
    if (!sock_in_progress()) {
        sock_close(fd);
        return -1;
    }
    return fd;
}

static bool connect_succeeded(int fd)
{
    int err = 0;
#ifdef _XBOX
    int len = sizeof(err);
#else
    socklen_t len = sizeof(err);
#endif

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0)
        return false;
    return err == 0;
}

int discover_hosts(const char *console_ip, char (*out)[16], int max, struct discover_progress *progress)
{
    struct probe probes[WINDOW];
    unsigned int a, b, c, d, next = 1;
    uint32_t prefix;
    int i, found = 0, active = 0, checked = 0;

    if (console_ip == NULL || sscanf(console_ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
        a > 255 || b > 255 || c > 255 || d == 0 || d > 254)
        return -1;
    prefix = (a << 24) | (b << 16) | (c << 8);
    if (progress) {
        progress->checked = 0;
        progress->total = 253;   /* .1 to .254 except ourselves */
        progress->found = 0;
    }
    for (i = 0; i < WINDOW; i++)
        probes[i].fd = -1;

    for (;;) {
        fd_set ws, es;
        struct timeval tv;
        uint32_t now;
        int maxfd = -1;

        /* Fill free slots. */
        for (i = 0; i < WINDOW && next <= 254; i++) {
            if (probes[i].fd >= 0)
                continue;
            while (next <= 254 && probes[i].fd < 0) {
                unsigned int host = next++;

                if (host == d)
                    continue;
                probes[i].fd = probe_start(prefix, host);
                probes[i].host = host;
                probes[i].started = plat_now_ms();
                if (probes[i].fd >= 0)
                    active++;
                else
                    checked++;
            }
        }
        if (progress)
            progress->checked = checked;
        if (active == 0)
            break;

        FD_ZERO(&ws);
        FD_ZERO(&es);
        for (i = 0; i < WINDOW; i++) {
            if (probes[i].fd < 0)
                continue;
            FD_SET(probes[i].fd, &ws);
            FD_SET(probes[i].fd, &es);
            if (probes[i].fd > maxfd)
                maxfd = probes[i].fd;
        }
        tv.tv_sec = 0;
        tv.tv_usec = 20 * 1000;
        select(maxfd + 1, NULL, &ws, &es, &tv);

        now = plat_now_ms();
        for (i = 0; i < WINDOW; i++) {
            struct probe *p = &probes[i];
            bool done = false;

            if (p->fd < 0)
                continue;
            if (FD_ISSET(p->fd, &es)) {
                done = true;   /* refused or unreachable */
            } else if (FD_ISSET(p->fd, &ws)) {
                done = true;
                if (connect_succeeded(p->fd) && found < max) {
                    snprintf(out[found], 16, "%u.%u.%u.%u", a, b, c, p->host);
                    found++;
                    if (progress)
                        progress->found = found;
                }
            } else if (now - p->started >= CONNECT_TIMEOUT_MS) {
                done = true;
            }
            if (done) {
                sock_close(p->fd);
                p->fd = -1;
                active--;
                checked++;
            }
        }
    }
    if (progress)
        progress->checked = checked;
    return found;
}
