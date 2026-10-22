/*
 * POSIX socket, poll/select, address and time functions for libxenon,
 * implemented on xsock and xthread.
 */
#define XENON_POSIX_IMPL 1

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>
#include <signal.h>
#include <sys/times.h>

#include "include/xenon_posix.h"
#include "include/sys/socket.h"
#include "include/netinet/in.h"
#include "include/arpa/inet.h"
#include "include/netdb.h"
#include "include/poll.h"
#include "include/sys/ioctl.h"
#include "xsock.h"
#include "xthread.h"

/* ------------------------------------------------------------ sockets */

int socket(int domain, int type, int protocol)
{
    return xsock_socket(domain, type, protocol);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    return xsock_bind(fd, addr, len);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return xsock_connect(fd, addr, len);
}

int listen(int fd, int backlog)
{
    return xsock_listen(fd, backlog);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return xsock_accept(fd, addr, len);
}

int shutdown(int fd, int how)
{
    return xsock_shutdown(fd, how);
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    return xsock_send(fd, buf, len, flags);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *addr, socklen_t alen)
{
    return xsock_sendto(fd, buf, len, flags, addr, alen);
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
    return xsock_recv(fd, buf, len, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *addr, socklen_t *alen)
{
    return xsock_recvfrom(fd, buf, len, flags, addr, alen);
}

int setsockopt(int fd, int level, int opt, const void *val, socklen_t len)
{
    return xsock_setsockopt(fd, level, opt, val, len);
}

int getsockopt(int fd, int level, int opt, void *val, socklen_t *len)
{
    return xsock_getsockopt(fd, level, opt, val, len);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    return xsock_getsockname(fd, addr, len);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    return xsock_getpeername(fd, addr, len);
}

int xenon_close(int fd)
{
    if (xsock_is_socket(fd))
        return xsock_close(fd);
    return close(fd);
}

int xenon_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg;

    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);

    if (!xsock_is_socket(fd)) {
        errno = ENOSYS;
        return -1;
    }
    switch (cmd) {
    case F_GETFL:
        return xsock_get_nonblocking(fd) > 0 ? O_NONBLOCK : 0;
    case F_SETFL:
        return xsock_set_nonblocking(fd, (arg & O_NONBLOCK) != 0);
    default:
        errno = EINVAL;
        return -1;
    }
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *argp;

    va_start(ap, request);
    argp = va_arg(ap, void *);
    va_end(ap);

    if (!xsock_is_socket(fd)) {
        errno = ENOTTY;
        return -1;
    }
    if (request == FIONBIO && argp)
        return xsock_set_nonblocking(fd, *(int *)argp);
    errno = EINVAL;
    return -1;
}

/* ------------------------------------------------------------ poll/select */

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    uint64_t deadline = timeout > 0 ? xthread_now_us() + (uint64_t)timeout * 1000 : 0;
    nfds_t i;

    for (;;) {
        int ready = 0;

        for (i = 0; i < nfds; i++) {
            int r, want;

            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            if (!xsock_is_socket(fds[i].fd)) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            r = xsock_ready(fds[i].fd);
            want = fds[i].events;
            if ((want & (POLLIN | POLLRDNORM)) && (r & XSOCK_READABLE))
                fds[i].revents |= want & (POLLIN | POLLRDNORM);
            if ((want & (POLLOUT | POLLWRNORM)) && (r & XSOCK_WRITABLE))
                fds[i].revents |= want & (POLLOUT | POLLWRNORM);
            /* POLLERR and POLLHUP are always reported. */
            if (r & XSOCK_ERROR)
                fds[i].revents |= POLLERR;
            if (r & XSOCK_HUP)
                fds[i].revents |= POLLHUP;
            if (fds[i].revents)
                ready++;
        }

        if (ready || timeout == 0)
            return ready;
        if (timeout > 0 && xthread_now_us() >= deadline)
            return 0;
        xthread_yield();
    }
}

int xenon_select(int nfds, fd_set *rs, fd_set *ws, fd_set *es, struct timeval *timeout)
{
    uint64_t deadline = 0;
    int fd;
    fd_set rin, win, ein;

    if (nfds > FD_SETSIZE)
        nfds = FD_SETSIZE;
    if (rs) rin = *rs; else FD_ZERO(&rin);
    if (ws) win = *ws; else FD_ZERO(&win);
    if (es) ein = *es; else FD_ZERO(&ein);
    if (timeout)
        deadline = xthread_now_us() + (uint64_t)timeout->tv_sec * 1000000 + timeout->tv_usec;

    for (;;) {
        int ready = 0;

        if (rs) FD_ZERO(rs);
        if (ws) FD_ZERO(ws);
        if (es) FD_ZERO(es);

        for (fd = XSOCK_FD_BASE; fd < nfds; fd++) {
            int r;
            bool want_r = FD_ISSET(fd, &rin), want_w = FD_ISSET(fd, &win), want_e = FD_ISSET(fd, &ein);

            if (!want_r && !want_w && !want_e)
                continue;
            r = xsock_ready(fd);
            if (want_r && (r & (XSOCK_READABLE | XSOCK_HUP | XSOCK_ERROR))) {
                FD_SET(fd, rs);
                ready++;
            }
            if (want_w && (r & XSOCK_WRITABLE)) {
                FD_SET(fd, ws);
                ready++;
            }
            if (want_e && (r & XSOCK_ERROR)) {
                FD_SET(fd, es);
                ready++;
            }
        }

        if (ready)
            return ready;
        if (timeout && xthread_now_us() >= deadline)
            return 0;
        xthread_yield();
    }
}

/* ------------------------------------------------------------ addresses */

int inet_aton(const char *cp, struct in_addr *inp)
{
    unsigned int parts[4];
    int n = 0;
    const char *p = cp;

    if (cp == NULL)
        return 0;
    while (n < 4) {
        unsigned int v = 0;
        int digits = 0;

        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (unsigned int)(*p - '0');
            if (v > 255)
                return 0;
            p++;
            digits++;
        }
        if (digits == 0)
            return 0;
        parts[n++] = v;
        if (*p == '.' && n < 4)
            p++;
        else
            break;
    }
    if (n != 4 || *p != '\0')
        return 0;
    if (inp)
        inp->s_addr = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

in_addr_t inet_addr(const char *cp)
{
    struct in_addr a;

    return inet_aton(cp, &a) ? a.s_addr : INADDR_NONE;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton(src, dst);
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const uint8_t *b = src;

    if (af != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    if (snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]) >= (int)size) {
        errno = ENOSPC;
        return NULL;
    }
    return dst;
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &in, buf, sizeof(buf));
    return buf;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct {
        struct addrinfo ai;
        struct sockaddr_in sin;
    } *r;
    struct in_addr addr;
    unsigned long port = 0;

    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;

    if (node == NULL) {
        addr.s_addr = (hints && (hints->ai_flags & AI_PASSIVE)) ? INADDR_ANY : INADDR_LOOPBACK;
    } else if (!inet_aton(node, &addr)) {
        /* No DNS client: only dotted-quad IPv4 addresses are accepted. */
        return EAI_NONAME;
    }

    if (service) {
        char *end;
        port = strtoul(service, &end, 10);
        if (*end != '\0' || port > 65535)
            return EAI_SERVICE;
    }

    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return EAI_MEMORY;

    r->sin.sin_family = AF_INET;
    r->sin.sin_port = htons((uint16_t)port);
    r->sin.sin_addr = addr;
    r->ai.ai_family = AF_INET;
    r->ai.ai_socktype = hints ? hints->ai_socktype : 0;
    r->ai.ai_protocol = hints ? hints->ai_protocol : 0;
    r->ai.ai_addrlen = sizeof(r->sin);
    r->ai.ai_addr = (struct sockaddr *)&r->sin;
    *res = &r->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);  /* addrinfo and its sockaddr share one allocation */
        res = next;
    }
}

const char *gai_strerror(int err)
{
    switch (err) {
    case EAI_NONAME:  return "Name does not resolve (only numeric IPv4 is supported)";
    case EAI_FAMILY:  return "Address family not supported";
    case EAI_SERVICE: return "Invalid service";
    case EAI_MEMORY:  return "Out of memory";
    default:          return "Address lookup failed";
    }
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen,
                char *serv, socklen_t servlen, int flags)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    (void)flags;

    if (sa == NULL || salen < sizeof(*sin) || sin->sin_family != AF_INET)
        return EAI_FAMILY;
    if (host && hostlen && inet_ntop(AF_INET, &sin->sin_addr, host, hostlen) == NULL)
        return EAI_OVERFLOW;
    if (serv && servlen && snprintf(serv, servlen, "%u", ntohs(sin->sin_port)) >= (int)servlen)
        return EAI_OVERFLOW;
    return 0;
}

/* ------------------------------------------------------------ time */

int xenon_usleep(useconds_t us)
{
    xthread_sleep_us(us);
    return 0;
}

unsigned int xenon_sleep(unsigned int seconds)
{
    xthread_sleep_us((uint64_t)seconds * 1000000);
    return 0;
}

int xenon_nanosleep(const struct timespec *req, struct timespec *rem)
{
    xthread_sleep_us((uint64_t)req->tv_sec * 1000000 + (uint64_t)req->tv_nsec / 1000);
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

int xenon_clock_gettime(clockid_t clk, struct timespec *ts)
{
    uint64_t us = xthread_now_us();
    (void)clk;

    /* Every clock is the monotonic timebase; nothing here needs wall time. */
    ts->tv_sec = (time_t)(us / 1000000);
    ts->tv_nsec = (long)(us % 1000000) * 1000;
    return 0;
}

int xenon_sched_yield(void)
{
    xthread_yield();
    return 0;
}

/* ------------------------------------------------------------ misc libc gaps */

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *p = memalign(alignment, size);

    if (p == NULL)
        return ENOMEM;
    *memptr = p;
    return 0;
}

/* There are no signals on bare metal; Moonlight only uses this to ignore SIGPIPE. */
int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    (void)sig;
    (void)act;
    if (old)
        memset(old, 0, sizeof(*old));
    return 0;
}

/* newlib's clock() calls times(); report the timebase as process time. */
clock_t times(struct tms *buf)
{
    clock_t t = (clock_t)(xthread_now_us() / (1000000 / CLOCKS_PER_SEC));

    if (buf) {
        buf->tms_utime = t;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return t;
}
