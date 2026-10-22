/*
 * Socket constants shared by the public BSD headers and xsock.c (which
 * can't include the BSD headers because they clash with lwIP's). Values
 * follow Linux.
 */
#ifndef XENON_NETCONST_H
#define XENON_NETCONST_H

#define AF_UNSPEC       0
#define AF_INET         2
#define PF_UNSPEC       AF_UNSPEC
#define PF_INET         AF_INET

#define SOCK_STREAM     1
#define SOCK_DGRAM      2

#define IPPROTO_IP      0
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_LINGER       13
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21

#define TCP_NODELAY     1
#define TCP_MAXSEG      2

#define IP_TOS          1
#define IP_TTL          2

#define MSG_PEEK        0x02
#define MSG_TRUNC       0x20
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000

#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#define INADDR_ANY       ((unsigned int)0x00000000)
#define INADDR_BROADCAST ((unsigned int)0xffffffff)
#define INADDR_NONE      ((unsigned int)0xffffffff)
#define INADDR_LOOPBACK  ((unsigned int)0x7f000001)

#define INET_ADDRSTRLEN 16

#endif
