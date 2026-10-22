/*
 * ENet platform layer for the Xbox 360 (replaces moonlight-common-c's
 * enet/win32.c). The console's Winsock is IPv4-only and has no
 * WSASendMsg/WSARecvMsg, so this follows ENet's Unix NO_MSGAPI path:
 * plain sendto/recvfrom, no QoS/ECN marking and no IP_PKTINFO.
 * XNetStartup() must have been called first (see platform_xdk.cpp).
 */
#define ENET_BUILDING_LIB 1
#include "enet/enet.h"

#include <stdlib.h>
#include <string.h>

static enet_uint32 timeBase = 0;

int enet_initialize(void)
{
    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData))
        return -1;
    return 0;
}

void enet_deinitialize(void)
{
    WSACleanup();
}

enet_uint32 enet_host_random_seed(void)
{
    return (enet_uint32)GetTickCount();
}

enet_uint32 enet_time_get(void)
{
    return (enet_uint32)GetTickCount() - timeBase;
}

void enet_time_set(enet_uint32 newTimeBase)
{
    timeBase = (enet_uint32)GetTickCount() - newTimeBase;
}

int enet_address_set_port(ENetAddress *address, enet_uint16 port)
{
    if (address->address.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&address->address;
        sin->sin_port = ENET_HOST_TO_NET_16(port);
        return 0;
    }
    return -1;
}

int enet_address_set_address(ENetAddress *address, struct sockaddr *addr, socklen_t addrlen)
{
    if ((size_t)addrlen > sizeof(struct sockaddr_storage))
        return -1;
    memcpy(&address->address, addr, addrlen);
    address->addressLength = addrlen;
    return 0;
}

int enet_address_equal(ENetAddress *address1, ENetAddress *address2)
{
    struct sockaddr_in *sin1, *sin2;

    if (address1->address.ss_family != AF_INET || address2->address.ss_family != AF_INET)
        return 0;
    sin1 = (struct sockaddr_in *)&address1->address;
    sin2 = (struct sockaddr_in *)&address2->address;
    return sin1->sin_port == sin2->sin_port && sin1->sin_addr.s_addr == sin2->sin_addr.s_addr;
}

int enet_address_wildcard(const ENetAddress *address)
{
    if (address->address.ss_family != AF_INET)
        return 0;
    return ((const struct sockaddr_in *)&address->address)->sin_addr.s_addr == INADDR_ANY;
}

int enet_address_set_host(ENetAddress *address, const char *name)
{
    struct addrinfo hints, *result = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &result) != 0 || result == NULL)
        return -1;
    memcpy(&address->address, result->ai_addr, result->ai_addrlen);
    address->addressLength = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return 0;
}

int enet_socket_bind(ENetSocket socket, const ENetAddress *address)
{
    return bind(socket, (const struct sockaddr *)&address->address, address->addressLength) == SOCKET_ERROR ? -1 : 0;
}

int enet_socket_get_address(ENetSocket socket, ENetAddress *address)
{
    address->addressLength = sizeof(address->address);
    if (getsockname(socket, (struct sockaddr *)&address->address, &address->addressLength) == SOCKET_ERROR)
        return -1;
    return 0;
}

int enet_socket_listen(ENetSocket socket, int backlog)
{
    return listen(socket, backlog < 0 ? SOMAXCONN : backlog) == SOCKET_ERROR ? -1 : 0;
}

ENetSocket enet_socket_create(int af, ENetSocketType type)
{
    if (af != AF_INET)
        return INVALID_SOCKET;
    return socket(AF_INET, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int enet_socket_set_option(ENetSocket socket, ENetSocketOption option, int value)
{
    int result = SOCKET_ERROR;

    switch (option) {
    case ENET_SOCKOPT_NONBLOCK: {
        u_long nonBlocking = (u_long)value;
        result = ioctlsocket(socket, FIONBIO, &nonBlocking);
        break;
    }
    case ENET_SOCKOPT_REUSEADDR:
        result = setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, (char *)&value, sizeof(int));
        break;
    case ENET_SOCKOPT_RCVBUF:
        result = setsockopt(socket, SOL_SOCKET, SO_RCVBUF, (char *)&value, sizeof(int));
        break;
    case ENET_SOCKOPT_SNDBUF:
        result = setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (char *)&value, sizeof(int));
        break;
    case ENET_SOCKOPT_RCVTIMEO:
        result = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&value, sizeof(int));
        break;
    case ENET_SOCKOPT_SNDTIMEO:
        result = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&value, sizeof(int));
        break;
    case ENET_SOCKOPT_NODELAY:
        result = setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)&value, sizeof(int));
        break;
    case ENET_SOCKOPT_QOS:
    case ENET_SOCKOPT_TTL:
        result = 0;   /* not supported on the console; harmless to ignore */
        break;
    default:
        break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
}

int enet_socket_get_option(ENetSocket socket, ENetSocketOption option, int *value)
{
    (void)socket;
    (void)option;
    *value = 0;
    return -1;   /* SO_ERROR / IP_TTL aren't available */
}

int enet_socket_connect(ENetSocket socket, const ENetAddress *address)
{
    if (connect(socket, (const struct sockaddr *)&address->address, address->addressLength) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK)
        return -1;
    return 0;
}

ENetSocket enet_socket_accept(ENetSocket socket, ENetAddress *address)
{
    SOCKET result;

    if (address != NULL)
        address->addressLength = sizeof(address->address);
    result = accept(socket, address != NULL ? (struct sockaddr *)&address->address : NULL,
                    address != NULL ? &address->addressLength : NULL);
    return result == INVALID_SOCKET ? ENET_SOCKET_NULL : result;
}

int enet_socket_shutdown(ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown(socket, (int)how) == SOCKET_ERROR ? -1 : 0;
}

void enet_socket_destroy(ENetSocket socket)
{
    if (socket != INVALID_SOCKET)
        closesocket(socket);
}

int enet_socket_send(ENetSocket socket, const ENetAddress *peerAddress, const ENetAddress *localAddress,
                     const ENetBuffer *buffers, size_t bufferCount)
{
    char *sendBuffer;
    size_t sendLength = 0, i;
    int sent;
    (void)localAddress;

    /* sendto() takes one buffer: gather multi-part packets first. */
    if (bufferCount == 1) {
        sendBuffer = (char *)buffers[0].data;
        sendLength = buffers[0].dataLength;
    } else {
        for (i = 0; i < bufferCount; i++)
            sendLength += buffers[i].dataLength;
        sendBuffer = (char *)malloc(sendLength);
        if (sendBuffer == NULL)
            return -1;
        sendLength = 0;
        for (i = 0; i < bufferCount; i++) {
            memcpy(sendBuffer + sendLength, buffers[i].data, buffers[i].dataLength);
            sendLength += buffers[i].dataLength;
        }
    }

    sent = sendto(socket, sendBuffer, (int)sendLength, 0,
                  (const struct sockaddr *)&peerAddress->address, peerAddress->addressLength);
    if (bufferCount != 1)
        free(sendBuffer);

    if (sent == SOCKET_ERROR) {
        switch (WSAGetLastError()) {
        case WSAEWOULDBLOCK:
        case WSAEADDRNOTAVAIL:
        case WSAENETDOWN:
        case WSAENETUNREACH:
        case WSAEHOSTDOWN:
        case WSAEHOSTUNREACH:
            return 0;   /* transient; ENet's timeouts handle real failures */
        default:
            return -1;
        }
    }
    return sent;
}

int enet_socket_receive(ENetSocket socket, ENetAddress *peerAddress, ENetAddress *localAddress,
                        ENetBuffer *buffers, size_t bufferCount)
{
    int fromLen = sizeof(peerAddress->address);
    int received;
    (void)bufferCount;   /* ENet always receives into a single buffer */

    received = recvfrom(socket, (char *)buffers[0].data, (int)buffers[0].dataLength, 0,
                        (struct sockaddr *)&peerAddress->address, &fromLen);
    if (received == SOCKET_ERROR) {
        switch (WSAGetLastError()) {
        case WSAEWOULDBLOCK:
        case WSAECONNRESET:
            return 0;
        case WSAEMSGSIZE:
            return -2;
        }
        return -1;
    }
    peerAddress->addressLength = fromLen;
    /* Without IP_PKTINFO, leave the local address to the socket's own binding. */
    if (localAddress != NULL)
        enet_socket_get_address(socket, localAddress);
    return received;
}

int enet_socketset_select(ENetSocket maxSocket, ENetSocketSet *readSet, ENetSocketSet *writeSet, enet_uint32 timeout)
{
    struct timeval timeVal;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;
    return select((int)maxSocket + 1, readSet, writeSet, NULL, &timeVal);
}

int enet_socket_wait(ENetSocket socket, enet_uint32 *condition, enet_uint32 timeout)
{
    fd_set readSet, writeSet;
    struct timeval timeVal;
    int selectCount;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    if (*condition & ENET_SOCKET_WAIT_SEND)
        FD_SET(socket, &writeSet);
    if (*condition & ENET_SOCKET_WAIT_RECEIVE)
        FD_SET(socket, &readSet);

    selectCount = select((int)socket + 1, &readSet, &writeSet, NULL, &timeVal);
    if (selectCount < 0)
        return -1;
    *condition = ENET_SOCKET_WAIT_NONE;
    if (selectCount == 0)
        return 0;
    if (FD_ISSET(socket, &writeSet))
        *condition |= ENET_SOCKET_WAIT_SEND;
    if (FD_ISSET(socket, &readSet))
        *condition |= ENET_SOCKET_WAIT_RECEIVE;
    return 0;
}
