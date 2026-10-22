/*
 * moonlight-common-c's crypto interface (src/PlatformCrypto.h) for the
 * Xbox 360 SDK build, on mbedTLS 2.16. Upstream implements it with OpenSSL
 * or mbedTLS 3's PSA API, neither of which builds with the SDK's compiler.
 *
 * Without USE_MBEDTLS, PLT_CRYPTO_CONTEXT holds an opaque EVP_CIPHER_CTX*;
 * we keep our own state behind that pointer instead.
 *
 * Semantics follow upstream's PSA implementation: GCM is one-shot per
 * message; CBC streams across calls until CIPHER_FLAG_FINISH or
 * CIPHER_FLAG_RESET_IV, with PKCS7 padding either by mbedTLS or (with
 * CIPHER_FLAG_PAD_TO_BLOCK_SIZE) added in place by us.
 */
#include "Limelight-internal.h"

#include <stdlib.h>
#include <string.h>
#include <mbedtls/cipher.h>
#include <mbedtls/gcm.h>

struct xdk_crypto {
    mbedtls_gcm_context gcm;
    mbedtls_cipher_context_t cbc;
    bool cbc_setup;
    bool cbc_active;
};

#define STATE(ctx) ((struct xdk_crypto *)(void *)(ctx)->ctx)

static int addPkcs7PaddingInPlace(unsigned char *plaintext, int plaintextLen)
{
    int paddedLength = ROUND_TO_PKCS7_PADDED_LEN(plaintextLen);
    unsigned char paddingByte = (unsigned char)(16 - (plaintextLen % 16));

    memset(&plaintext[plaintextLen], paddingByte, paddedLength - plaintextLen);
    return paddedLength;
}

static bool cbc_crypt(PPLT_CRYPTO_CONTEXT ctx, mbedtls_operation_t op, int flags,
                      unsigned char *key, int keyLength, unsigned char *iv, int ivLength,
                      unsigned char *in, int inLen, unsigned char *out, int *outLen)
{
    struct xdk_crypto *st = STATE(ctx);
    size_t olen = 0, flen = 0;

    if (!ctx->initialized) {
        const mbedtls_cipher_info_t *info = mbedtls_cipher_info_from_values(
            MBEDTLS_CIPHER_ID_AES, keyLength * 8, MBEDTLS_MODE_CBC);
        if (info == NULL || mbedtls_cipher_setup(&st->cbc, info) != 0)
            return false;
        st->cbc_setup = true;
        if (mbedtls_cipher_setkey(&st->cbc, key, keyLength * 8, op) != 0)
            return false;
        if (mbedtls_cipher_set_padding_mode(&st->cbc, (flags & CIPHER_FLAG_PAD_TO_BLOCK_SIZE) ?
                                            MBEDTLS_PADDING_NONE : MBEDTLS_PADDING_PKCS7) != 0)
            return false;
        ctx->initialized = true;
    }

    if (!st->cbc_active || (flags & CIPHER_FLAG_RESET_IV)) {
        if (mbedtls_cipher_set_iv(&st->cbc, iv, ivLength) != 0 || mbedtls_cipher_reset(&st->cbc) != 0)
            return false;
        st->cbc_active = true;
    }

    if (op == MBEDTLS_ENCRYPT && (flags & CIPHER_FLAG_PAD_TO_BLOCK_SIZE))
        inLen = addPkcs7PaddingInPlace(in, inLen);

    if (mbedtls_cipher_update(&st->cbc, in, inLen, out, &olen) != 0) {
        st->cbc_active = false;
        return false;
    }
    if (flags & CIPHER_FLAG_FINISH) {
        st->cbc_active = false;
        if (mbedtls_cipher_finish(&st->cbc, out + olen, &flen) != 0)
            return false;
        olen += flen;
    }
    *outLen = (int)olen;
    return true;
}

static bool gcm_setkey(PPLT_CRYPTO_CONTEXT ctx, unsigned char *key, int keyLength)
{
    if (!ctx->initialized) {
        if (mbedtls_gcm_setkey(&STATE(ctx)->gcm, MBEDTLS_CIPHER_ID_AES, key, keyLength * 8) != 0)
            return false;
        ctx->initialized = true;
    }
    return true;
}

bool PltEncryptMessage(PPLT_CRYPTO_CONTEXT ctx, int algorithm, int flags,
                       unsigned char *key, int keyLength,
                       unsigned char *iv, int ivLength,
                       unsigned char *tag, int tagLength,
                       unsigned char *inputData, int inputDataLength,
                       unsigned char *outputData, int *outputDataLength)
{
    if (algorithm == ALGORITHM_AES_GCM) {
        if (!gcm_setkey(ctx, key, keyLength))
            return false;
        if (mbedtls_gcm_crypt_and_tag(&STATE(ctx)->gcm, MBEDTLS_GCM_ENCRYPT, inputDataLength, iv, ivLength,
                                      NULL, 0, inputData, outputData, tagLength, tag) != 0)
            return false;
        *outputDataLength = inputDataLength;
        return true;
    }
    if (algorithm == ALGORITHM_AES_CBC)
        return cbc_crypt(ctx, MBEDTLS_ENCRYPT, flags, key, keyLength, iv, ivLength,
                         inputData, inputDataLength, outputData, outputDataLength);
    return false;
}

bool PltDecryptMessage(PPLT_CRYPTO_CONTEXT ctx, int algorithm, int flags,
                       unsigned char *key, int keyLength,
                       unsigned char *iv, int ivLength,
                       unsigned char *tag, int tagLength,
                       unsigned char *inputData, int inputDataLength,
                       unsigned char *outputData, int *outputDataLength)
{
    if (algorithm == ALGORITHM_AES_GCM) {
        if (!gcm_setkey(ctx, key, keyLength))
            return false;
        if (mbedtls_gcm_auth_decrypt(&STATE(ctx)->gcm, inputDataLength, iv, ivLength, NULL, 0,
                                     tag, tagLength, inputData, outputData) != 0)
            return false;
        *outputDataLength = inputDataLength;
        return true;
    }
    if (algorithm == ALGORITHM_AES_CBC)
        return cbc_crypt(ctx, MBEDTLS_DECRYPT, flags, key, keyLength, iv, ivLength,
                         inputData, inputDataLength, outputData, outputDataLength);
    return false;
}

PPLT_CRYPTO_CONTEXT PltCreateCryptoContext(void)
{
    PPLT_CRYPTO_CONTEXT ctx = (PPLT_CRYPTO_CONTEXT)calloc(1, sizeof(*ctx));
    struct xdk_crypto *st;

    if (ctx == NULL)
        return NULL;
    st = (struct xdk_crypto *)calloc(1, sizeof(*st));
    if (st == NULL) {
        free(ctx);
        return NULL;
    }
    mbedtls_gcm_init(&st->gcm);
    mbedtls_cipher_init(&st->cbc);
    ctx->ctx = (EVP_CIPHER_CTX *)(void *)st;
    ctx->initialized = false;
    return ctx;
}

void PltDestroyCryptoContext(PPLT_CRYPTO_CONTEXT ctx)
{
    struct xdk_crypto *st = STATE(ctx);

    mbedtls_gcm_free(&st->gcm);
    mbedtls_cipher_free(&st->cbc);
    free(st);
    free(ctx);
}

void PltGenerateRandomData(unsigned char *data, int length)
{
    XNetRandom(data, (unsigned int)length);
}
