/*
 * mbedTLS 2.28 configuration additions for the Xbox 360 SDK build
 * (passed as MBEDTLS_USER_CONFIG_FILE on top of the default config).
 */

/* No BSD sockets, filesystem or wall-clock date handling for mbedTLS. */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_HAVE_TIME_DATE

/* Entropy comes from the console's hardware RNG (XeCryptRandom) through
 * mbedtls_hardware_poll() in xdk/src/platform_xdk.cpp. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* Server side, DTLS and threading aren't used. */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_THREADING_C
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_PADLOCK_C
