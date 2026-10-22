/*
 * mbedTLS user config for libxenon (passed as MBEDTLS_USER_CONFIG_FILE).
 * Trims the default config down to what pairing, HTTPS and the stream
 * crypto need, and removes everything that expects an OS.
 */

/* No BSD sockets, /dev/urandom, filesystem or wall-clock for mbedTLS. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
/* mbedtls_ms_time() is implemented in src/crypto.c from the PPC timebase. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* TLS 1.2 is all GameStream hosts need; 1.3 would pull in more PSA code. */
#undef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE
#undef MBEDTLS_SSL_EARLY_DATA

/* Not used, and some assume an OS. */
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_THREADING_C
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C
