/*
 * <winsock2.h>/<ws2tcpip.h> for the Xbox 360. The console's Winsock
 * (<winsockx.h>, pulled in by <xtl.h>) is an IPv4-only subset; this adds the
 * pieces desktop code relies on: sockaddr_storage, getaddrinfo (numeric),
 * inet_ntop/inet_pton, struct pollfd. Implemented in compat/win_net.c.
 */
#ifndef XDK_WINSOCK2_H
#define XDK_WINSOCK2_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int socklen_t;

struct sockaddr_storage {
    short ss_family;
    char __ss_pad1[6];
    __int64 __ss_align;
    char __ss_pad2[112];
};
typedef struct sockaddr_storage SOCKADDR_STORAGE;

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    char *ai_canonname;
    struct sockaddr *ai_addr;
    struct addrinfo *ai_next;
};
typedef struct addrinfo ADDRINFOA;

#define AI_PASSIVE      0x01
#define AI_CANONNAME    0x02
#define AI_NUMERICHOST  0x04
#define AI_ADDRCONFIG   0x0400
#define AI_NUMERICSERV  0x0008
#define EAI_NONAME      11001
#define EAI_FAMILY      10047
#define EAI_SERVICE     10109
#define EAI_MEMORY      8

#define INET_ADDRSTRLEN  16

int WSAAPI getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void WSAAPI freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int err);
const char *WSAAPI inet_ntop(int af, const void *src, char *dst, size_t size);
int WSAAPI inet_pton(int af, const char *src, void *dst);

/* poll() emulation types; moonlight-common-c implements poll with select(). */
#ifndef POLLIN
#define POLLRDNORM  0x0100
#define POLLRDBAND  0x0200
#define POLLIN      (POLLRDNORM | POLLRDBAND)
#define POLLPRI     0x0400
#define POLLWRNORM  0x0010
#define POLLOUT     POLLWRNORM
#define POLLERR     0x0001
#define POLLHUP     0x0002
#define POLLNVAL    0x0004
typedef struct pollfd {
    SOCKET fd;
    short events;
    short revents;
} WSAPOLLFD;
#endif

/* Desktop-only options: setsockopt/getsockopt/WSAIoctl simply fail with these. */
#ifndef SO_ERROR
#define SO_ERROR          0x1007
#endif
#ifndef IP_MTU_DISCOVER
#define IP_MTU_DISCOVER   71
#define IP_PMTUDISC_DONT  0
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6      41
#define IPV6_MTU_DISCOVER 71
#endif
#ifndef IOC_VENDOR
#define IOC_VENDOR 0x18000000
#define _WSAIOW(x, y) (IOC_IN | (x) | (y))
#endif
#ifndef IOC_IN
#define IOC_IN 0x80000000
#endif
/*
 * Titles may normally only use Xbox Live's secure sockets. Every socket is
 * created through xdk_socket(), which marks it insecure so it can talk plain
 * TCP/UDP to the PC (needs the InsecureSockets privilege in xex.xml and
 * XNET_STARTUP_BYPASS_SECURITY).
 */
SOCKET WSAAPI xdk_socket(int af, int type, int protocol);
#define socket(af, type, protocol) xdk_socket(af, type, protocol)

int WSAAPI WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len,
                    LPDWORD bytes, LPVOID overlapped, LPVOID completion);

#ifdef __cplusplus
}
#endif

#endif
