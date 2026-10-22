/*
 * BSD sockets over the lwIP 1.4 raw API. See xsock.h.
 *
 * Everything here runs on hardware thread 0. lwIP callbacks fire from
 * network_poll(), which the xthread scheduler calls on every pass, so no
 * locking is needed: callbacks and socket calls never run concurrently.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/opt.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <lwip/tcp.h>
#include <lwip/netif.h>
#include <network/network.h>

#include "xsock.h"
#include "xthread.h"
#include "include/xenon_netconst.h"

/* Same layout as struct sockaddr_in in include/netinet/in.h. */
struct xs_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;      /* network order */
    uint32_t sin_addr;      /* network order */
    uint8_t sin_zero[8];
};

#define XSOCK_ACCEPT_QUEUE 8

#define UDP_SLOT_SIZE    2048
#define UDP_RING_DEFAULT 64
#define UDP_RING_MAX     2048

struct udp_slot {
    uint32_t addr;
    uint16_t port;
    uint16_t len;
    bool truncated;
    uint8_t data[UDP_SLOT_SIZE];
};

struct xsock {
    bool used;
    int type;
    bool nonblocking;
    int pending_error;          /* SO_ERROR */
    uint64_t rcvtimeo_us;       /* 0 = wait forever */
    uint64_t sndtimeo_us;
    bool shut_rd, shut_wr;

    /* UDP */
    struct udp_pcb *udp;
    struct udp_slot *ring;
    int ring_cap, ring_head, ring_count;
    int ring_want;              /* requested capacity (SO_RCVBUF) */
    uint32_t drops;

    /* TCP */
    struct tcp_pcb *tcp;
    struct pbuf *rx;            /* queued received data */
    u16_t rx_off;               /* bytes already consumed from rx head */
    bool connecting, connected, peer_closed;
    bool nodelay;

    /* TCP listening: accepted connections already have their own slot (data
     * may arrive before accept() is called); their fds wait here. */
    bool listening;
    int accept_queue[XSOCK_ACCEPT_QUEUE];
    int accept_count;
};

static struct xsock socks[XSOCK_MAX];

static struct xsock *get_sock(int fd)
{
    int idx = fd - XSOCK_FD_BASE;

    if (idx < 0 || idx >= XSOCK_MAX || !socks[idx].used) {
        errno = EBADF;
        return NULL;
    }
    return &socks[idx];
}

static int fd_of(struct xsock *s)
{
    return (int)(s - socks) + XSOCK_FD_BASE;
}

int xsock_is_socket(int fd)
{
    int idx = fd - XSOCK_FD_BASE;
    return idx >= 0 && idx < XSOCK_MAX && socks[idx].used;
}

uint32_t xsock_local_ip(void)
{
    return netif.ip_addr.addr;
}

int xsock_init(void)
{
    memset(socks, 0, sizeof(socks));
    return 0;
}

void xsock_pump(void)
{
    network_poll();
}

/* Waits (yielding) until done() is true or the deadline passes. */
static int wait_until(struct xsock *s, bool (*done)(struct xsock *), uint64_t timeout_us)
{
    uint64_t deadline = timeout_us ? xthread_now_us() + timeout_us : 0;

    while (!done(s)) {
        if (deadline && xthread_now_us() >= deadline) {
            errno = EAGAIN;
            return -1;
        }
        xthread_yield();
    }
    return 0;
}

static int map_lwip_err(err_t err)
{
    switch (err) {
    case ERR_OK:        return 0;
    case ERR_MEM:       return ENOMEM;
    case ERR_BUF:       return ENOBUFS;
    case ERR_TIMEOUT:   return ETIMEDOUT;
    case ERR_RTE:       return EHOSTUNREACH;
    case ERR_INPROGRESS:return EINPROGRESS;
    case ERR_VAL:       return EINVAL;
    case ERR_WOULDBLOCK:return EWOULDBLOCK;
    case ERR_USE:       return EADDRINUSE;
    case ERR_ISCONN:    return EISCONN;
    case ERR_ABRT:      return ECONNABORTED;
    case ERR_RST:       return ECONNRESET;
    case ERR_CLSD:      return ENOTCONN;
    case ERR_CONN:      return ENOTCONN;
    case ERR_ARG:       return EINVAL;
    case ERR_IF:        return ENETDOWN;
    default:            return EIO;
    }
}

/* ---------------------------------------------------------------- UDP */

static bool udp_ring_alloc(struct xsock *s)
{
    int cap = s->ring_want ? s->ring_want : UDP_RING_DEFAULT;

    if (s->ring && s->ring_cap == cap)
        return true;
    if (s->ring_count != 0)
        return s->ring != NULL; /* don't resize with data queued */
    free(s->ring);
    s->ring = malloc(sizeof(struct udp_slot) * cap);
    if (s->ring == NULL) {
        s->ring_cap = 0;
        return false;
    }
    s->ring_cap = cap;
    s->ring_head = 0;
    s->ring_count = 0;
    return true;
}

static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, ip_addr_t *addr, u16_t port)
{
    struct xsock *s = arg;
    struct udp_slot *slot;
    (void)pcb;

    if (s->ring == NULL || s->ring_count == s->ring_cap || s->shut_rd) {
        s->drops++;
        pbuf_free(p);
        return;
    }

    slot = &s->ring[(s->ring_head + s->ring_count) % s->ring_cap];
    slot->addr = addr->addr;
    slot->port = port;
    slot->truncated = p->tot_len > UDP_SLOT_SIZE;
    slot->len = pbuf_copy_partial(p, slot->data, UDP_SLOT_SIZE, 0);
    s->ring_count++;
    pbuf_free(p);
}

/* ---------------------------------------------------------------- TCP */

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct xsock *s = arg;
    (void)pcb;

    if (s == NULL) {
        if (p)
            pbuf_free(p);
        return ERR_OK;
    }
    if (p == NULL) {
        s->peer_closed = true;
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    if (s->rx)
        pbuf_cat(s->rx, p);
    else
        s->rx = p;
    return ERR_OK;
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct xsock *s = arg;
    (void)pcb;

    if (s) {
        s->connecting = false;
        if (err == ERR_OK)
            s->connected = true;
        else
            s->pending_error = map_lwip_err(err);
    }
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    struct xsock *s = arg;

    if (s == NULL)
        return;
    /* lwIP has already freed the pcb. */
    s->tcp = NULL;
    if (s->connecting) {
        /* A reset during the handshake means nobody is listening. */
        s->pending_error = (err == ERR_RST) ? ECONNREFUSED : map_lwip_err(err);
        s->connecting = false;
    } else {
        s->pending_error = map_lwip_err(err);
    }
    s->peer_closed = true;
}

static void tcp_detach_pcb(struct xsock *s)
{
    if (s->tcp) {
        tcp_arg(s->tcp, NULL);
        tcp_recv(s->tcp, NULL);
        tcp_err(s->tcp, NULL);
        tcp_sent(s->tcp, NULL);
        if (tcp_close(s->tcp) != ERR_OK)
            tcp_abort(s->tcp);
        s->tcp = NULL;
    }
    if (s->rx) {
        pbuf_free(s->rx);
        s->rx = NULL;
    }
}

/* ---------------------------------------------------------------- API */

int xsock_socket(int domain, int type, int protocol)
{
    struct xsock *s = NULL;
    int i;
    (void)protocol;

    if (domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    for (i = 0; i < XSOCK_MAX; i++) {
        if (!socks[i].used) {
            s = &socks[i];
            break;
        }
    }
    if (s == NULL) {
        errno = EMFILE;
        return -1;
    }

    memset(s, 0, sizeof(*s));
    s->type = type;

    if (type == SOCK_DGRAM) {
        s->udp = udp_new();
        if (s->udp == NULL) {
            errno = ENOMEM;
            return -1;
        }
        if (!udp_ring_alloc(s)) {
            udp_remove(s->udp);
            errno = ENOMEM;
            return -1;
        }
        udp_recv(s->udp, udp_recv_cb, s);
    } else {
        s->tcp = tcp_new();
        if (s->tcp == NULL) {
            errno = ENOMEM;
            return -1;
        }
        tcp_arg(s->tcp, s);
        tcp_recv(s->tcp, tcp_recv_cb);
        tcp_err(s->tcp, tcp_err_cb);
    }

    s->used = true;
    return fd_of(s);
}

int xsock_close(int fd)
{
    struct xsock *s = get_sock(fd);

    if (s == NULL)
        return -1;
    if (s->type == SOCK_DGRAM) {
        if (s->udp)
            udp_remove(s->udp);
        free(s->ring);
    } else if (s->listening) {
        int i, queued[XSOCK_ACCEPT_QUEUE], n = s->accept_count;

        if (s->tcp) {
            tcp_arg(s->tcp, NULL);
            tcp_accept(s->tcp, NULL);
            tcp_close(s->tcp);
            s->tcp = NULL;
        }
        memcpy(queued, s->accept_queue, n * sizeof(int));
        memset(s, 0, sizeof(*s));
        for (i = 0; i < n; i++)
            xsock_close(queued[i]);   /* never accepted */
        return 0;
    } else {
        tcp_detach_pcb(s);
    }
    memset(s, 0, sizeof(*s));
    return 0;
}

static bool parse_addr(const void *addr, unsigned int addrlen, ip_addr_t *ip, u16_t *port)
{
    const struct xs_sockaddr_in *sin = addr;

    if (addr == NULL || addrlen < sizeof(*sin) || sin->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return false;
    }
    ip->addr = sin->sin_addr;
    *port = ntohs(sin->sin_port);
    return true;
}

static int fill_addr(void *addr, unsigned int *addrlen, uint32_t ip, u16_t port)
{
    struct xs_sockaddr_in sin;

    if (addr == NULL || addrlen == NULL)
        return 0;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr = ip;
    memcpy(addr, &sin, *addrlen < sizeof(sin) ? *addrlen : sizeof(sin));
    *addrlen = sizeof(sin);
    return 0;
}

int xsock_bind(int fd, const void *addr, unsigned int addrlen)
{
    struct xsock *s = get_sock(fd);
    ip_addr_t ip;
    u16_t port;
    err_t err;

    if (s == NULL || !parse_addr(addr, addrlen, &ip, &port))
        return -1;

    if (s->type == SOCK_DGRAM)
        err = udp_bind(s->udp, &ip, port);
    else if (s->tcp)
        err = tcp_bind(s->tcp, &ip, port);
    else
        err = ERR_CLSD;

    if (err != ERR_OK) {
        errno = map_lwip_err(err);
        return -1;
    }
    return 0;
}

static bool tcp_connect_done(struct xsock *s)
{
    return !s->connecting;
}

int xsock_connect(int fd, const void *addr, unsigned int addrlen)
{
    struct xsock *s = get_sock(fd);
    ip_addr_t ip;
    u16_t port;
    err_t err;

    if (s == NULL || !parse_addr(addr, addrlen, &ip, &port))
        return -1;

    if (s->type == SOCK_DGRAM) {
        err = udp_connect(s->udp, &ip, port);
        if (err != ERR_OK) {
            errno = map_lwip_err(err);
            return -1;
        }
        return 0;
    }

    if (s->tcp == NULL) {
        errno = s->pending_error ? s->pending_error : EBADF;
        return -1;
    }
    if (s->connected) {
        errno = EISCONN;
        return -1;
    }
    if (s->connecting) {
        errno = EALREADY;
        return -1;
    }

    s->connecting = true;
    s->pending_error = 0;
    err = tcp_connect(s->tcp, &ip, port, tcp_connected_cb);
    if (err != ERR_OK) {
        s->connecting = false;
        errno = map_lwip_err(err);
        return -1;
    }

    if (s->nonblocking) {
        errno = EINPROGRESS;
        return -1;
    }

    wait_until(s, tcp_connect_done, 0);
    if (!s->connected) {
        errno = s->pending_error ? s->pending_error : ECONNREFUSED;
        s->pending_error = 0;
        return -1;
    }
    return 0;
}

/* A new connection on a listening socket: give it a socket slot right away
 * (so data arriving before accept() is buffered) and queue it. */
static err_t tcp_accept_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    struct xsock *l = arg, *s = NULL;
    int i;

    if (l == NULL || pcb == NULL || err != ERR_OK)
        return ERR_VAL;
    tcp_accepted(l->tcp);
    if (l->accept_count < XSOCK_ACCEPT_QUEUE) {
        for (i = 0; i < XSOCK_MAX; i++) {
            if (!socks[i].used) {
                s = &socks[i];
                break;
            }
        }
    }
    if (s == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    memset(s, 0, sizeof(*s));
    s->used = true;
    s->type = SOCK_STREAM;
    s->tcp = pcb;
    s->connected = true;
    /* Servers send many small messages; don't let Nagle hold them back. */
    tcp_nagle_disable(pcb);
    s->nodelay = true;
    tcp_arg(pcb, s);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_err(pcb, tcp_err_cb);
    l->accept_queue[l->accept_count++] = fd_of(s);
    return ERR_OK;
}

int xsock_listen(int fd, int backlog)
{
    struct xsock *s = get_sock(fd);
    struct tcp_pcb *l;
    (void)backlog;

    if (s == NULL)
        return -1;
    if (s->type != SOCK_STREAM || s->tcp == NULL || s->connected || s->connecting) {
        errno = EINVAL;
        return -1;
    }
    if (s->listening)
        return 0;

    l = tcp_listen(s->tcp);   /* frees the original pcb */
    if (l == NULL) {
        errno = ENOMEM;
        return -1;
    }
    s->tcp = l;
    s->listening = true;
    tcp_arg(l, s);
    tcp_accept(l, tcp_accept_cb);
    return 0;
}

static bool accept_ready(struct xsock *s)
{
    return s->accept_count > 0 || s->tcp == NULL;
}

int xsock_accept(int fd, void *addr, unsigned int *addrlen)
{
    struct xsock *s = get_sock(fd), *c;
    int cfd;

    if (s == NULL)
        return -1;
    if (!s->listening) {
        errno = EINVAL;
        return -1;
    }
    if (s->accept_count == 0) {
        if (s->nonblocking) {
            errno = EAGAIN;
            return -1;
        }
        wait_until(s, accept_ready, 0);
        if (s->accept_count == 0) {
            errno = EBADF;
            return -1;
        }
    }

    cfd = s->accept_queue[0];
    s->accept_count--;
    memmove(s->accept_queue, s->accept_queue + 1, s->accept_count * sizeof(int));
    c = get_sock(cfd);
    if (c && c->tcp)
        fill_addr(addr, addrlen, c->tcp->remote_ip.addr, c->tcp->remote_port);
    else
        fill_addr(addr, addrlen, 0, 0);
    return cfd;
}

int xsock_shutdown(int fd, int how)
{
    struct xsock *s = get_sock(fd);

    if (s == NULL)
        return -1;
    if (how == SHUT_RD || how == SHUT_RDWR)
        s->shut_rd = true;
    if (how == SHUT_WR || how == SHUT_RDWR)
        s->shut_wr = true;
    if (s->type == SOCK_STREAM && s->tcp && s->connected)
        tcp_shutdown(s->tcp, how != SHUT_WR, how != SHUT_RD);
    return 0;
}

static bool tcp_can_send(struct xsock *s)
{
    return s->tcp == NULL || s->shut_wr || tcp_sndbuf(s->tcp) > 0;
}

static int tcp_send_data(struct xsock *s, const uint8_t *buf, size_t len, int flags)
{
    size_t sent = 0;
    bool nonblock = s->nonblocking || (flags & MSG_DONTWAIT);

    while (sent < len) {
        u16_t room;
        size_t chunk;
        err_t err;

        if (s->tcp == NULL || !s->connected) {
            errno = s->pending_error ? s->pending_error : ENOTCONN;
            return sent ? (int)sent : -1;
        }
        if (s->shut_wr) {
            errno = EPIPE;
            return sent ? (int)sent : -1;
        }

        room = tcp_sndbuf(s->tcp);
        if (room == 0) {
            tcp_output(s->tcp);
            if (nonblock) {
                if (sent)
                    return (int)sent;
                errno = EAGAIN;
                return -1;
            }
            if (wait_until(s, tcp_can_send, s->sndtimeo_us) < 0)
                return sent ? (int)sent : -1;
            continue;
        }

        chunk = len - sent;
        if (chunk > room)
            chunk = room;
        err = tcp_write(s->tcp, buf + sent, chunk, TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) {
            /* Out of segments: let lwIP drain and retry. */
            tcp_output(s->tcp);
            if (nonblock) {
                if (sent)
                    return (int)sent;
                errno = EAGAIN;
                return -1;
            }
            xthread_yield();
            continue;
        }
        if (err != ERR_OK) {
            errno = map_lwip_err(err);
            return sent ? (int)sent : -1;
        }
        sent += chunk;
    }

    tcp_output(s->tcp);
    return (int)sent;
}

static int udp_send_data(struct xsock *s, const void *buf, size_t len, const ip_addr_t *ip, u16_t port)
{
    struct pbuf *p;
    err_t err;

    if (len > 0xFFFF - 28) {
        errno = EMSGSIZE;
        return -1;
    }
    p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (p == NULL) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(p->payload, buf, len);
    if (ip)
        err = udp_sendto(s->udp, p, (ip_addr_t *)ip, port);
    else
        err = udp_send(s->udp, p);
    pbuf_free(p);
    if (err != ERR_OK) {
        errno = map_lwip_err(err);
        return -1;
    }
    return (int)len;
}

int xsock_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, unsigned int addrlen)
{
    struct xsock *s = get_sock(fd);
    ip_addr_t ip;
    u16_t port;

    if (s == NULL)
        return -1;
    if (s->type == SOCK_STREAM)
        return tcp_send_data(s, buf, len, flags);
    if (addr == NULL)
        return udp_send_data(s, buf, len, NULL, 0);
    if (!parse_addr(addr, addrlen, &ip, &port))
        return -1;
    return udp_send_data(s, buf, len, &ip, port);
}

int xsock_send(int fd, const void *buf, size_t len, int flags)
{
    return xsock_sendto(fd, buf, len, flags, NULL, 0);
}

static bool udp_has_data(struct xsock *s)
{
    return s->ring_count > 0 || s->shut_rd;
}

static bool tcp_has_data(struct xsock *s)
{
    return s->rx != NULL || s->peer_closed || s->shut_rd || s->tcp == NULL;
}

int xsock_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, unsigned int *addrlen)
{
    struct xsock *s = get_sock(fd);
    bool nonblock;

    if (s == NULL)
        return -1;
    nonblock = s->nonblocking || (flags & MSG_DONTWAIT);

    if (s->type == SOCK_DGRAM) {
        struct udp_slot *slot;
        size_t n;

        if (s->ring_count == 0) {
            if (s->shut_rd)
                return 0;
            if (nonblock) {
                errno = EAGAIN;
                return -1;
            }
            if (wait_until(s, udp_has_data, s->rcvtimeo_us) < 0)
                return -1;
            if (s->ring_count == 0)
                return 0;
        }

        slot = &s->ring[s->ring_head];
        n = slot->len < len ? slot->len : len;
        memcpy(buf, slot->data, n);
        fill_addr(addr, addrlen, slot->addr, slot->port);
        if (!(flags & MSG_PEEK)) {
            s->ring_head = (s->ring_head + 1) % s->ring_cap;
            s->ring_count--;
        }
        return (int)n;
    }

    /* TCP */
    if (s->rx == NULL) {
        if (s->shut_rd || s->peer_closed)
            return 0;
        if (s->tcp == NULL) {
            errno = s->pending_error ? s->pending_error : ENOTCONN;
            return -1;
        }
        if (nonblock) {
            errno = EAGAIN;
            return -1;
        }
        if (wait_until(s, tcp_has_data, s->rcvtimeo_us) < 0)
            return -1;
        if (s->rx == NULL) {
            if (s->pending_error && !s->peer_closed) {
                errno = s->pending_error;
                return -1;
            }
            return 0;
        }
    }

    {
        u16_t want = len > 0xFFFF ? 0xFFFF : (u16_t)len;
        u16_t n = pbuf_copy_partial(s->rx, buf, want, s->rx_off);

        if (!(flags & MSG_PEEK)) {
            /* Advance the read offset and release fully consumed pbufs. */
            s->rx_off += n;
            while (s->rx && s->rx_off >= s->rx->len) {
                struct pbuf *head = s->rx;
                struct pbuf *next = head->next;

                s->rx_off -= head->len;
                if (next)
                    pbuf_ref(next);   /* keep the rest alive past pbuf_free(head) */
                pbuf_free(head);
                s->rx = next;
            }
            if (s->tcp)
                tcp_recved(s->tcp, n);
        }
        if (addr && addrlen && s->tcp)
            fill_addr(addr, addrlen, s->tcp->remote_ip.addr, s->tcp->remote_port);
        return n;
    }
}

int xsock_recv(int fd, void *buf, size_t len, int flags)
{
    return xsock_recvfrom(fd, buf, len, flags, NULL, NULL);
}

static uint64_t timeval_to_us(const void *val, unsigned int len)
{
    /* struct timeval on newlib/ppc32: time_t (64-bit) + suseconds_t (long). */
    if (len >= 12) {
        const int64_t *sec = val;
        const int32_t *usec = (const int32_t *)((const uint8_t *)val + 8);
        return (uint64_t)*sec * 1000000 + (uint64_t)*usec;
    }
    if (len >= 8) {
        const int32_t *v = val;
        return (uint64_t)v[0] * 1000000 + (uint64_t)v[1];
    }
    return 0;
}

int xsock_setsockopt(int fd, int level, int opt, const void *val, unsigned int len)
{
    struct xsock *s = get_sock(fd);
    int ival = (val && len >= sizeof(int)) ? *(const int *)val : 0;

    if (s == NULL)
        return -1;

    if (level == SOL_SOCKET) {
        switch (opt) {
        case SO_RCVBUF:
            if (s->type == SOCK_DGRAM) {
                int cap = ival / 1500;
                if (cap < UDP_RING_DEFAULT)
                    cap = UDP_RING_DEFAULT;
                if (cap > UDP_RING_MAX)
                    cap = UDP_RING_MAX;
                s->ring_want = cap;
                if (!udp_ring_alloc(s)) {
                    errno = ENOBUFS;
                    return -1;
                }
            }
            return 0;
        case SO_RCVTIMEO:
            s->rcvtimeo_us = timeval_to_us(val, len);
            return 0;
        case SO_SNDTIMEO:
            s->sndtimeo_us = timeval_to_us(val, len);
            return 0;
        case SO_REUSEADDR:
        case SO_SNDBUF:
        case SO_BROADCAST:
        case SO_KEEPALIVE:
        case SO_LINGER:
            return 0;
        }
    } else if (level == IPPROTO_TCP && s->type == SOCK_STREAM) {
        switch (opt) {
        case TCP_NODELAY:
            s->nodelay = ival != 0;
            if (s->tcp) {
                if (ival)
                    tcp_nagle_disable(s->tcp);
                else
                    tcp_nagle_enable(s->tcp);
            }
            return 0;
        case TCP_MAXSEG:
            return 0;
        }
    } else if (level == IPPROTO_IP && opt == IP_TOS) {
        if (s->type == SOCK_DGRAM && s->udp)
            s->udp->tos = (u8_t)ival;
        else if (s->tcp)
            s->tcp->tos = (u8_t)ival;
        return 0;
    } else if (level == IPPROTO_IP && opt == IP_TTL) {
        if (s->type == SOCK_DGRAM && s->udp)
            s->udp->ttl = (u8_t)ival;
        else if (s->tcp)
            s->tcp->ttl = (u8_t)ival;
        return 0;
    }

    errno = ENOPROTOOPT;
    return -1;
}

int xsock_getsockopt(int fd, int level, int opt, void *val, unsigned int *len)
{
    struct xsock *s = get_sock(fd);

    if (s == NULL)
        return -1;
    if (val == NULL || len == NULL || *len < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }
    if (level == SOL_SOCKET && opt == SO_ERROR) {
        *(int *)val = s->pending_error;
        s->pending_error = 0;
        *len = sizeof(int);
        return 0;
    }
    if (level == SOL_SOCKET && opt == SO_RCVBUF) {
        *(int *)val = s->type == SOCK_DGRAM ? s->ring_cap * 1500 : TCP_WND;
        *len = sizeof(int);
        return 0;
    }
    errno = ENOPROTOOPT;
    return -1;
}

int xsock_getsockname(int fd, void *addr, unsigned int *addrlen)
{
    struct xsock *s = get_sock(fd);
    uint32_t ip = 0;
    u16_t port = 0;

    if (s == NULL)
        return -1;
    if (s->type == SOCK_DGRAM && s->udp) {
        ip = s->udp->local_ip.addr;
        port = s->udp->local_port;
    } else if (s->tcp) {
        ip = s->tcp->local_ip.addr;
        port = s->tcp->local_port;
    }
    if (ip == 0)
        ip = xsock_local_ip();
    return fill_addr(addr, addrlen, ip, port);
}

int xsock_getpeername(int fd, void *addr, unsigned int *addrlen)
{
    struct xsock *s = get_sock(fd);

    if (s == NULL)
        return -1;
    if (s->type == SOCK_DGRAM && s->udp && (s->udp->flags & UDP_FLAGS_CONNECTED))
        return fill_addr(addr, addrlen, s->udp->remote_ip.addr, s->udp->remote_port);
    if (s->type == SOCK_STREAM && s->tcp && s->connected)
        return fill_addr(addr, addrlen, s->tcp->remote_ip.addr, s->tcp->remote_port);
    errno = ENOTCONN;
    return -1;
}

int xsock_set_nonblocking(int fd, int enabled)
{
    struct xsock *s = get_sock(fd);

    if (s == NULL)
        return -1;
    s->nonblocking = enabled != 0;
    return 0;
}

int xsock_get_nonblocking(int fd)
{
    struct xsock *s = get_sock(fd);

    if (s == NULL)
        return -1;
    return s->nonblocking;
}

int xsock_ready(int fd)
{
    struct xsock *s = get_sock(fd);
    int r = 0;

    if (s == NULL)
        return XSOCK_ERROR;

    if (s->type == SOCK_DGRAM) {
        if (s->ring_count > 0 || s->shut_rd)
            r |= XSOCK_READABLE;
        r |= XSOCK_WRITABLE;
        return r;
    }
    if (s->listening)
        return s->accept_count > 0 ? XSOCK_READABLE : 0;

    if (s->rx || s->peer_closed || s->shut_rd)
        r |= XSOCK_READABLE;
    if (s->peer_closed)
        r |= XSOCK_HUP;
    if (s->pending_error)
        r |= XSOCK_ERROR;
    if (s->connected && s->tcp && tcp_sndbuf(s->tcp) > 0)
        r |= XSOCK_WRITABLE;
    /* A finished (failed) connect reports writable+error, like Linux. */
    if (!s->connecting && !s->connected && s->pending_error)
        r |= XSOCK_WRITABLE;
    return r;
}
