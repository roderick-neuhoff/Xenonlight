#ifndef XENON_COMPAT_NETINET_IN_H
#define XENON_COMPAT_NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

/* IPv6 types for code that declares them; lwIP here is IPv4-only. */
struct in6_addr {
    uint8_t s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

/* The Xenon CPU is big-endian, so network order is host order. */
#ifndef htons
#define htons(x) ((uint16_t)(x))
#define ntohs(x) ((uint16_t)(x))
#define htonl(x) ((uint32_t)(x))
#define ntohl(x) ((uint32_t)(x))
#endif

#endif
