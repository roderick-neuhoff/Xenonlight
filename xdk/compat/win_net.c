/*
 * Desktop Winsock functions the Xbox 360's Winsock lacks. Name resolution is
 * numeric IPv4 only (the app asks for the host's IP address).
 */
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_ipv4(const char *s, struct in_addr *out)
{
    unsigned long a = inet_addr(s);

    /* inet_addr returns INADDR_NONE for both errors and "255.255.255.255". */
    if (a == INADDR_NONE && strcmp(s, "255.255.255.255") != 0)
        return 0;
    out->s_addr = a;
    return 1;
}

int WSAAPI getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    struct result {
        struct addrinfo ai;
        struct sockaddr_in sin;
    } *r;
    struct in_addr addr;
    unsigned long port = 0;

    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;
    if (node == NULL)
        addr.s_addr = (hints && (hints->ai_flags & AI_PASSIVE)) ? INADDR_ANY : htonl(INADDR_LOOPBACK);
    else if (!parse_ipv4(node, &addr))
        return EAI_NONAME;
    if (service) {
        char *end;
        port = strtoul(service, &end, 10);
        if (*end != '\0' || port > 65535)
            return EAI_SERVICE;
    }

    r = (struct result *)calloc(1, sizeof(*r));
    if (r == NULL)
        return EAI_MEMORY;
    r->sin.sin_family = AF_INET;
    r->sin.sin_port = htons((u_short)port);
    r->sin.sin_addr = addr;
    r->ai.ai_family = AF_INET;
    r->ai.ai_socktype = hints ? hints->ai_socktype : 0;
    r->ai.ai_protocol = hints ? hints->ai_protocol : 0;
    r->ai.ai_addrlen = sizeof(r->sin);
    r->ai.ai_addr = (struct sockaddr *)&r->sin;
    *res = &r->ai;
    return 0;
}

void WSAAPI freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);   /* the sockaddr lives in the same allocation */
        res = next;
    }
}

const char *gai_strerror(int err)
{
    switch (err) {
    case EAI_NONAME:  return "Only numeric IPv4 addresses are supported";
    case EAI_FAMILY:  return "Address family not supported";
    case EAI_SERVICE: return "Invalid port";
    case EAI_MEMORY:  return "Out of memory";
    default:          return "Address lookup failed";
    }
}

const char *WSAAPI inet_ntop(int af, const void *src, char *dst, size_t size)
{
    const unsigned char *b = (const unsigned char *)src;

    if (af != AF_INET || size < INET_ADDRSTRLEN)
        return NULL;
    _snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    dst[size - 1] = '\0';
    return dst;
}

int WSAAPI inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET)
        return -1;
    return parse_ipv4(src, (struct in_addr *)dst);
}

int WSAAPI WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len,
                    LPDWORD bytes, LPVOID overlapped, LPVOID completion)
{
    (void)s; (void)code; (void)in; (void)in_len; (void)out; (void)out_len;
    (void)bytes; (void)overlapped; (void)completion;
    WSASetLastError(WSAEOPNOTSUPP);
    return SOCKET_ERROR;
}

#undef socket

/* Undocumented socket options used by Xbox 360 homebrew to allow plain
 * (non-VDP) traffic; harmless where the kernel already allows it. */
#define SO_XBOX_INSECURE_1 0x5801
#define SO_XBOX_INSECURE_2 0x5802

SOCKET WSAAPI xdk_socket(int af, int type, int protocol)
{
    SOCKET s = socket(af, type, protocol);
    BOOL on = TRUE;

    if (s != INVALID_SOCKET) {
        setsockopt(s, SOL_SOCKET, SO_XBOX_INSECURE_1, (const char *)&on, sizeof(on));
        setsockopt(s, SOL_SOCKET, SO_XBOX_INSECURE_2, (const char *)&on, sizeof(on));
    }
    return s;
}
