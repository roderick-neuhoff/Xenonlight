#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "crypto.h"

struct http_response {
    int status;
    char *body;     /* NUL-terminated, caller frees */
    size_t len;
};

/*
 * Performs GET http(s)://host:port/path_and_query. HTTPS presents the
 * client identity. timeout_ms bounds each wait for data (pairing waits on
 * the user typing a PIN, so it needs a long one). Returns false on
 * network/TLS failure; HTTP errors are reported via status.
 */
bool http_get(const char *host, int port, bool https, const char *path,
              const struct client_identity *id, int timeout_ms, struct http_response *out);
void http_response_free(struct http_response *r);
