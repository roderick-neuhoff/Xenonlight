#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

/* Seeds the shared DRBG. Must run before any other crypto call. */
bool crypto_init(void);
mbedtls_ctr_drbg_context *crypto_drbg(void);
void crypto_random(void *out, size_t len);

/*
 * Client identity used for pairing and HTTPS. Loaded from storage, or
 * generated (RSA-2048, takes a while on Xenon) and saved on first run.
 */
struct client_identity {
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    char *cert_pem;     /* NUL-terminated */
};

bool crypto_load_identity(struct client_identity *id, void (*status)(const char *msg));
