/*
 * xsock: BSD-style sockets implemented on the lwIP 1.4 raw API.
 *
 * Only usable from xthread coroutines on hardware thread 0. Blocking calls
 * yield to other coroutines (which also pumps lwIP) until they can proceed.
 * Address arguments use the Linux sockaddr_in layout (see include/sys/socket.h).
 * All functions return -1 and set errno on failure.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * libxenon file descriptors occupy 3..1026, so sockets start above them.
 * The build sets FD_SETSIZE >= XSOCK_FD_BASE + XSOCK_MAX for select().
 */
#define XSOCK_FD_BASE 2048
#define XSOCK_MAX     64

int  xsock_init(void);   /* call after network_init() */
void xsock_pump(void);   /* lwIP poll; registered with xthread_add_pump */

int xsock_is_socket(int fd);
int xsock_socket(int domain, int type, int protocol);
int xsock_close(int fd);
int xsock_bind(int fd, const void *addr, unsigned int addrlen);
int xsock_connect(int fd, const void *addr, unsigned int addrlen);
int xsock_listen(int fd, int backlog);
int xsock_accept(int fd, void *addr, unsigned int *addrlen);
int xsock_shutdown(int fd, int how);
int xsock_send(int fd, const void *buf, size_t len, int flags);
int xsock_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned int addrlen);
int xsock_recv(int fd, void *buf, size_t len, int flags);
int xsock_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, unsigned int *addrlen);
int xsock_setsockopt(int fd, int level, int opt, const void *val, unsigned int len);
int xsock_getsockopt(int fd, int level, int opt, void *val, unsigned int *len);
int xsock_getsockname(int fd, void *addr, unsigned int *addrlen);
int xsock_getpeername(int fd, void *addr, unsigned int *addrlen);
int xsock_set_nonblocking(int fd, int enabled);
int xsock_get_nonblocking(int fd);

/* Readiness bits (match POLLIN/POLLOUT/POLLERR/POLLHUP in include/poll.h). */
#define XSOCK_READABLE 0x01
#define XSOCK_WRITABLE 0x04
#define XSOCK_ERROR    0x08
#define XSOCK_HUP      0x10
int xsock_ready(int fd);

/* IPv4 address of the console's interface, network byte order. */
uint32_t xsock_local_ip(void);
