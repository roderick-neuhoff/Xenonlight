#include "crypto.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/entropy.h>
#include <mbedtls/hmac_drbg.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>

#ifdef _XBOX
/* Xbox 360 SDK build: mbedTLS 2.16, real OS threads, XNet for randomness. */
#  include <xtl.h>
#else
#  include <xb360/xb360.h>
#  include "../compat/xthread.h"
#  include <mbedtls/platform_time.h>
#  include <ppc/timebase.h>
#  include <xenon_nand/xenon_config.h>
#endif

#define CERT_FILE "client.pem"
#define KEY_FILE  "key.pem"

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;

/* Called by mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT), which is C even when this
 * file is compiled as C++ (SDK build). */
#ifdef __cplusplus
extern "C"
#endif
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);

#ifdef _XBOX

/* Entropy source for MBEDTLS_ENTROPY_HARDWARE_ALT: the system's RNG. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    if (XNetRandom(output, (UINT)len) != 0)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = len;
    return 0;
}

/* Threads are preemptive here, so nothing needs to yield. */
static void rng_yield(void)
{
}

/* The console's Ethernet MAC address. */
static void get_mac(unsigned char mac[6])
{
    XNADDR addr;

    memset(mac, 0, 6);
    if (XNetGetTitleXnAddr(&addr) != XNET_GET_XNADDR_PENDING)
        memcpy(mac, addr.abEnet, 6);
}

static uint64_t timestamp(void)
{
    LARGE_INTEGER t;

    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

/* A title can't read the CPU key, so the identity is random and kept in
 * storage (the app's folder, which is always writable from the dashboard). */
static bool derive_console_seed(unsigned char seed[32])
{
    (void)seed;
    return false;
}

#else

/*
 * Entropy source for MBEDTLS_ENTROPY_HARDWARE_ALT. The console exposes no
 * hardware RNG to libxenon, so this harvests timebase jitter from memory
 * and cache timing. Its quality is unproven, but it only has to protect
 * pairing nonces and the per-session AES keys on a local network.
 */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    static volatile uint32_t scratch[4096];
    size_t i;
    (void)data;

    for (i = 0; i < len; i++) {
        uint8_t acc = 0;
        int bit;

        for (bit = 0; bit < 8; bit++) {
            uint64_t t0 = mftb();
            uint32_t j, n = (uint32_t)(t0 & 0x3F) + 16;

            for (j = 0; j < n; j++)
                scratch[(j * 97 + (uint32_t)t0) & 4095] += j;
            acc = (uint8_t)((acc << 1) | ((mftb() - t0) & 1));
        }
        output[i] = acc;
    }
    *olen = len;
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(mftb() / (PPC_TIMEBASE_FREQ / 1000));
}

/* RSA key generation runs for seconds without any other reason to yield to
 * the other coroutines; this keeps the UI's spinner moving while it does. */
static void rng_yield(void)
{
    xthread_yield();
}

static void get_mac(unsigned char mac[6])
{
    xenon_config_get_mac_addr(mac);
}

static uint64_t timestamp(void)
{
    return mftb();
}

/*
 * Seed for the console's deterministic client identity: SHA-256 over a fixed
 * label and the per-console CPU key from the eFuses. The key never leaves the
 * console and can't be recovered from the seed or the certificate.
 */
static bool derive_console_seed(unsigned char seed[32])
{
    static const char label[] = "Moonlight-Xbox360 client identity v1";
    unsigned char cpu_key[16];
    mbedtls_sha256_context sha;
    bool all_zero = true, all_ones = true;
    int i;

    cpu_get_key(cpu_key);
    for (i = 0; i < 16; i++) {
        all_zero &= cpu_key[i] == 0x00;
        all_ones &= cpu_key[i] == 0xFF;
    }
    if (all_zero || all_ones)
        return false;   /* fuses unreadable */

    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, (const unsigned char *)label, sizeof(label) - 1);
    mbedtls_sha256_update(&sha, cpu_key, sizeof(cpu_key));
    mbedtls_sha256_finish(&sha, seed);
    mbedtls_sha256_free(&sha);
    memset(cpu_key, 0, sizeof(cpu_key));
    return true;
}

#endif

static int hmac_rng_yield(void *p, unsigned char *out, size_t len)
{
    rng_yield();
    return mbedtls_hmac_drbg_random(p, out, len);
}

static int ctr_rng_yield(void *p, unsigned char *out, size_t len)
{
    rng_yield();
    return mbedtls_ctr_drbg_random(p, out, len);
}

bool crypto_init(void)
{
    unsigned char personal[16];
    uint64_t tb = timestamp();

    /* Personalise with the console's MAC so two consoles never share a stream. */
    memset(personal, 0, sizeof(personal));
    get_mac(personal);
    memcpy(personal + 8, &tb, sizeof(tb));

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    return mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, personal, sizeof(personal)) == 0;
}

mbedtls_ctr_drbg_context *crypto_drbg(void)
{
    return &drbg;
}

void crypto_random(void *out, size_t len)
{
    mbedtls_ctr_drbg_random(&drbg, (unsigned char *)out, len);
}

/*
 * Creates a key and self-signed cert as malloc'd, NUL-terminated PEM strings.
 * All randomness comes from f_rng, so a deterministic generator yields the
 * identical certificate every time (PKCS#1 v1.5 signatures are deterministic;
 * the RNG passed to signing is only used for blinding).
 */
typedef int (*rng_fn)(void *, unsigned char *, size_t);

static bool generate_identity(rng_fn f_rng, void *p_rng, char **cert_pem, char **key_pem)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    unsigned char serial[8];
    unsigned char *buf;
    bool ok = false;
    const size_t buf_size = 8192;

    *cert_pem = *key_pem = NULL;
    buf = (unsigned char *)malloc(buf_size);
    if (buf == NULL)
        return false;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0)
        goto out;
    if (mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), f_rng, p_rng, 2048, 65537) != 0)
        goto out;

    if (f_rng(p_rng, serial, sizeof(serial)) != 0)
        goto out;
    serial[0] &= 0x7F;  /* keep the serial positive */

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=NVIDIA GameStream Client") != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&crt, "CN=NVIDIA GameStream Client") != 0)
        goto out;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
#if MBEDTLS_VERSION_MAJOR >= 3
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0)
        goto out;
#else
    {
        mbedtls_mpi sn;
        int r;

        mbedtls_mpi_init(&sn);
        r = mbedtls_mpi_read_binary(&sn, serial, sizeof(serial));
        if (r == 0)
            r = mbedtls_x509write_crt_set_serial(&crt, &sn);
        mbedtls_mpi_free(&sn);
        if (r != 0)
            goto out;
    }
#endif
    if (mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20440101000000") != 0)
        goto out;

    if (mbedtls_x509write_crt_pem(&crt, buf, buf_size, f_rng, p_rng) != 0)
        goto out;
    *cert_pem = strdup((char *)buf);

    if (mbedtls_pk_write_key_pem(&key, buf, buf_size) != 0)
        goto out;
    *key_pem = strdup((char *)buf);

    ok = *cert_pem && *key_pem;

out:
    if (!ok) {
        free(*cert_pem);
        free(*key_pem);
        *cert_pem = *key_pem = NULL;
    }
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    memset(buf, 0, buf_size);
    free(buf);
    return ok;
}

bool crypto_load_identity(struct client_identity *id, void (*status)(const char *msg))
{
    size_t cert_len, key_len;
    char *key_pem;

    mbedtls_x509_crt_init(&id->cert);
    mbedtls_pk_init(&id->key);
    id->cert_pem = storage_read(CERT_FILE, &cert_len);
    key_pem = storage_read(KEY_FILE, &key_len);

    if (id->cert_pem == NULL || key_pem == NULL) {
        unsigned char seed[32];
        bool from_fuses, ok;

        free(id->cert_pem);
        free(key_pem);

        from_fuses = derive_console_seed(seed);
        if (from_fuses) {
            /* Same console -> same seed -> same certificate, so a pairing
             * survives reboots even without a USB stick. */
            mbedtls_hmac_drbg_context hmac;

            if (status)
                status("Creating this console's client certificate (same every boot, takes a moment)...");
            mbedtls_hmac_drbg_init(&hmac);
            ok = mbedtls_hmac_drbg_seed_buf(&hmac, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), seed, sizeof(seed)) == 0 &&
                 generate_identity(hmac_rng_yield, &hmac, &id->cert_pem, &key_pem);
            mbedtls_hmac_drbg_free(&hmac);
            memset(seed, 0, sizeof(seed));
        } else {
            if (status)
                status("Generating client certificate (RSA-2048). This can take a minute...");
            ok = generate_identity(ctr_rng_yield, &drbg, &id->cert_pem, &key_pem);
        }
        if (!ok) {
            if (status)
                status("Could not generate the client certificate.");
            return false;
        }

        cert_len = strlen(id->cert_pem);
        key_len = strlen(key_pem);
        /* Saving just skips regeneration next boot (and is required when the
         * fuses couldn't be read, or pairing is lost at power-off). */
        if (!storage_write(CERT_FILE, id->cert_pem, cert_len) || !storage_write(KEY_FILE, key_pem, key_len)) {
            if (status && !from_fuses)
                status("Couldn't save the certificate to USB; you'll need to pair again after a reboot.");
        }
    }

    /* mbedTLS needs the trailing NUL counted in PEM lengths. */
    if (mbedtls_x509_crt_parse(&id->cert, (unsigned char *)id->cert_pem, cert_len + 1) != 0 ||
#if MBEDTLS_VERSION_MAJOR >= 3
        mbedtls_pk_parse_key(&id->key, (unsigned char *)key_pem, key_len + 1, NULL, 0,
                             mbedtls_ctr_drbg_random, &drbg) != 0) {
#else
        mbedtls_pk_parse_key(&id->key, (unsigned char *)key_pem, key_len + 1, NULL, 0) != 0) {
#endif
        free(key_pem);
        if (status)
            status("Stored client certificate is unreadable. Delete client.pem and key.pem to regenerate.");
        return false;
    }
    memset(key_pem, 0, key_len);
    free(key_pem);
    return true;
}
