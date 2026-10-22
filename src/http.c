#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _XBOX
#  include <winsock2.h>
#  define sock_close closesocket
#  define sock_again() (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAETIMEDOUT)
#else
#  include <errno.h>
#  include <strings.h>
#  include <unistd.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  define sock_close close
#  define sock_again() (errno == EAGAIN)
#endif

#include <mbedtls/ssl.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>   /* only for the MBEDTLS_ERR_NET_* codes */

#define MAX_RESPONSE (4 * 1024 * 1024)

struct conn {
    int fd;
    bool tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
};

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int n = (int)send(fd, (const char *)buf, (int)len, 0);

    if (n < 0)
        return sock_again() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int n = (int)recv(fd, (char *)buf, (int)len, 0);

    if (n < 0)
        return sock_again() ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
    return n;
}

static int tcp_connect(const char *host, int port, int timeout_ms)
{
    struct sockaddr_in sa;
    int fd, one = 1;
#ifdef _XBOX
    DWORD tv = (DWORD)timeout_ms;   /* Winsock takes milliseconds */
#else
    struct timeval tv;
#endif

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (sa.sin_addr.s_addr == INADDR_NONE)
        return -1;

    fd = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#ifndef _XBOX
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        sock_close(fd);
        return -1;
    }
    return fd;
}

static bool conn_open(struct conn *c, const char *host, int port, bool https,
                      const struct client_identity *id, int timeout_ms)
{
    int ret;

    memset(c, 0, sizeof(*c));
    c->fd = tcp_connect(host, port, timeout_ms);
    if (c->fd < 0)
        return false;
    if (!https)
        return true;

    c->tls = true;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return false;
    /*
     * GameStream hosts use self-signed certificates that are exchanged during
     * pairing; we do not pin it yet, so the server certificate isn't checked.
     */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, crypto_drbg());
    if (id && mbedtls_ssl_conf_own_cert(&c->conf, (mbedtls_x509_crt *)&id->cert, (mbedtls_pk_context *)&id->key) != 0)
        return false;
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0)
        return false;
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char err[128];
            mbedtls_strerror(ret, err, sizeof(err));
            printf("TLS handshake with %s:%d failed: %s\n", host, port, err);
            return false;
        }
    }
    return true;
}

static void conn_close(struct conn *c)
{
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
    }
    if (c->fd >= 0)
        sock_close(c->fd);
    c->fd = -1;
}

static int conn_write(struct conn *c, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        int n = c->tls ? mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + off, len - off)
                       : (int)send(c->fd, buf + off, (int)(len - off), 0);
        if (c->tls && (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ))
            continue;
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return (int)len;
}

/* Returns bytes read, 0 on orderly close, -1 on error/timeout. */
static int conn_read(struct conn *c, char *buf, size_t len)
{
    for (;;) {
        int n = c->tls ? mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len)
                       : (int)recv(c->fd, buf, (int)len, 0);
        if (!c->tls)
            return n;
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0)
            return 0;
        return n < 0 ? -1 : n;
    }
}

/* strcasestr, which not every C library has. */
static const char *find_nocase(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);

    for (; *haystack; haystack++)
        if (strncasecmp(haystack, needle, n) == 0)
            return haystack;
    return NULL;
}

/* Decodes a chunked body in place. Returns the new length or -1. */
static long dechunk(char *body, size_t len)
{
    char *in = body, *out = body, *end = body + len;

    while (in < end) {
        char *eol;
        unsigned long size = strtoul(in, &eol, 16);

        eol = strstr(in, "\r\n");
        if (eol == NULL)
            return -1;
        in = eol + 2;
        if (size == 0)
            break;
        if (in + size > end)
            return -1;
        memmove(out, in, size);
        out += size;
        in += size + 2;
    }
    *out = '\0';
    return out - body;
}

bool http_get(const char *host, int port, bool https, const char *path,
              const struct client_identity *id, int timeout_ms, struct http_response *out)
{
    struct conn c;
    char *req, *buf, *hdr_end, *body;
    size_t cap = 16384, len = 0, req_len;
    long content_length = -1;
    bool chunked = false;
    int n;

    memset(out, 0, sizeof(*out));
    if (!conn_open(&c, host, port, https, id, timeout_ms)) {
        conn_close(&c);
        return false;
    }

    req_len = strlen(path) + strlen(host) + 128;
    req = (char *)malloc(req_len);
    buf = (char *)malloc(cap + 1);
    if (req == NULL || buf == NULL)
        goto fail;
    snprintf(req, req_len,
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: Moonlight-Xbox360\r\nConnection: close\r\n\r\n",
             path, host, port);
    if (conn_write(&c, req, strlen(req)) < 0)
        goto fail;

    /* Read the whole response; the host closes the connection when done. */
    for (;;) {
        if (len == cap) {
            char *nb;
            if (cap >= MAX_RESPONSE)
                goto fail;
            cap *= 2;
            nb = (char *)realloc(buf, cap + 1);
            if (nb == NULL)
                goto fail;
            buf = nb;
        }
        n = conn_read(&c, buf + len, cap - len);
        if (n < 0)
            goto fail;
        if (n == 0)
            break;
        len += (size_t)n;
        buf[len] = '\0';

        /* Stop early once Content-Length bytes have arrived. */
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end && content_length < 0 && !chunked) {
            const char *cl = find_nocase(buf, "\r\nContent-Length:");
            if (cl && cl < hdr_end)
                content_length = strtol(cl + 17, NULL, 10);
            chunked = find_nocase(buf, "\r\nTransfer-Encoding: chunked") != NULL;
        }
        if (hdr_end && content_length >= 0 && (size_t)(hdr_end + 4 - buf) + (size_t)content_length <= len)
            break;
    }
    buf[len] = '\0';

    if (sscanf(buf, "HTTP/%*d.%*d %d", &out->status) != 1)
        goto fail;
    hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end == NULL)
        goto fail;
    body = hdr_end + 4;
    out->len = len - (size_t)(body - buf);
    if (content_length >= 0 && (size_t)content_length < out->len)
        out->len = (size_t)content_length;
    memmove(buf, body, out->len);
    buf[out->len] = '\0';
    if (chunked) {
        long l = dechunk(buf, out->len);
        if (l < 0)
            goto fail;
        out->len = (size_t)l;
    }
    out->body = buf;

    free(req);
    conn_close(&c);
    return true;

fail:
    free(req);
    free(buf);
    conn_close(&c);
    return false;
}

void http_response_free(struct http_response *r)
{
    free(r->body);
    r->body = NULL;
    r->len = 0;
}
